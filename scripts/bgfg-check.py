# SPDX-License-Identifier: Apache-2.0
# bgfg-check.py <run dir> <behaviour>: scripts/bgfg.sh's check of one run of the background/foreground probe. It
# compares the run's app.log with the engine's dylib.log, checks the three assertions in bgfg.sh's header and the
# run's other requirements, and prints "PASS <info>" or "FAIL <reason>".
import os
import re
import sys
from dataclasses import dataclass
from typing import Optional

APP_LINE = re.compile(r'.*?PROBE\| (\d+\.\d+)\s+\S+ (.*)')
ENGINE_LINE = re.compile(r'(\d{9}\.\d{3}) \[simcam \d+\] (.*)')
CALL_LINE = re.compile(r'CALL (start|stop)Running \((.*?)\) \| before: (.*)')
RETURN_LINE = re.compile(r'RET  (start|stop)Running \((.*?)\)')
SESSION_RUNNING_LINE = re.compile(r'session (0x[0-9a-f]+) (Start|Stop)Running')
INTERRUPTION_ENDED_LINE = re.compile(r'interruption ended session (0x[0-9a-f]+)')
INTERRUPT_LINE = re.compile(r'interrupt session (0x[0-9a-f]+)')

# The engine log is appended to and named by pid. Its lines from more than this long before the probe's first line
# come from an earlier process that had the same pid.
EARLIER_PROCESS_S = 1.0
# The two logs are written with millisecond timestamps by different processes; times this close count as the same.
CLOCK_SLACK_S = 0.005
# An engine line may be logged up to this long after the call it belongs to returned.
AFTER_RETURN_SLACK_S = 0.05
# The only redundant call a behaviour makes is the launch-time start (stopInBackground and appNotif). More would hide
# a dropped call.
EXPECTED_REDUNDANT = {'stopInBackground': 1, 'keepRunning': 0, 'stopWhileInterrupted': 0, 'appNotif': 1}
# The behaviours that stop the session while it is interrupted, and so must show that happening.
INTERRUPTED_STOP_BEHAVIOURS = ('stopWhileInterrupted', 'appNotif')
ENGINE_LINE_NAMES = {"start": "StartRunning", "stop": "StopRunning", "ended": "interruption ended"}


@dataclass
class Call:
    """One startRunning or stopRunning call the probe made."""
    time: float
    kind: str  # 'start' or 'stop'
    why: str
    returned: Optional[float]  # the time of its RET line, or None if it never returned
    before: str  # the probe's state before the call


@dataclass
class Event:
    """One session event in the engine log."""
    time: float
    kind: str  # 'start', 'stop', 'ended' (interruption ended) or 'interrupt'
    session: str


def read_lines(run_dir, name):
    """Returns the lines of a file in the run directory, or no lines if it is missing."""
    path = os.path.join(run_dir, name)
    return open(path, errors='replace').read().splitlines() if os.path.exists(path) else []


def timed_lines(lines, pattern):
    """Returns (time, text) for each line that matches the pattern."""
    found = []
    for line in lines:
        match = pattern.match(line)
        if match:
            found.append((float(match.group(1)), match.group(2)))

    return found


def missing_setup(app, engine):
    """Returns what the run lacks to be checked at all: the probe's END, both scene transitions, the engine log."""
    missing = []
    for what, text in (('END', 'END |'), ('didEnterBackground', 'SCENE didEnterBackground')):
        if not any(text in line for _, line in app):
            missing.append(what)

    # The scene's first willEnterForeground is at launch; the second is the return from the background.
    if sum('SCENE willEnterForeground' in line for _, line in app) < 2:
        missing.append('willEnterForeground after the background')

    if not engine:
        missing.append('engine log')

    return missing


def probe_calls(app):
    """Returns the probe's calls in order, each with the time of its RET line if it returned."""
    calls = []
    for time, line in app:
        match = CALL_LINE.match(line)
        if match:
            calls.append(Call(time, match.group(1), match.group(2), None, match.group(3)))

        match = RETURN_LINE.match(line)
        if match:
            for call in reversed(calls):
                if call.kind == match.group(1) and call.why == match.group(2) and call.returned is None:
                    call.returned = time
                    break

    return calls


def session_events(engine):
    """Returns the engine's session events in log order."""
    events = []
    for time, line in engine:
        match = SESSION_RUNNING_LINE.match(line)
        if match:
            events.append(Event(time, match.group(2).lower(), match.group(1)))
            continue

        match = INTERRUPTION_ENDED_LINE.match(line)
        if match:
            events.append(Event(time, 'ended', match.group(1)))
            continue

        match = INTERRUPT_LINE.match(line)
        if match:
            events.append(Event(time, 'interrupt', match.group(1)))

    return events


def check_calls_reach_engine(calls, runs, end_time, start_time, fails):
    """Assertion (a): every call that changes what the app asked for reaches the engine, in order.

    Each call takes the first unused engine StartRunning/StopRunning line at or after it. That line must be of the
    call's kind and come before the call returned (or before the next call, if it never returned).

    A call of the same kind as the app's previous one is redundant (the probe's launch-time start in the first
    willEnterForeground, while already running): AVFoundation drops a start on a running session itself, so it need
    not reach the engine. A call that changes what the app asked for must reach it, whatever isRunning reads. After an
    unasked restart, AVFoundation reads isRunning=1 and drops the app's start: that is the bug.

    Appends failures to fails. Returns (the session the calls went to or None, the number of redundant calls).
    """
    next_run = 0
    session = None
    redundant = 0
    for index, call in enumerate(calls):
        if call.returned is not None:
            deadline = call.returned + AFTER_RETURN_SLACK_S
        elif index + 1 < len(calls):
            deadline = calls[index + 1].time
        else:
            deadline = end_time

        while next_run < len(runs) and runs[next_run].time < call.time - CLOCK_SLACK_S:
            next_run += 1

        if next_run < len(runs) and runs[next_run].kind == call.kind and runs[next_run].time <= deadline:
            session = session or runs[next_run].session
            next_run += 1
        elif index > 0 and calls[index - 1].kind == call.kind:
            redundant += 1
        else:
            if next_run < len(runs):
                run = runs[next_run]
                next_line = f'next engine line {run.kind} at {run.time - call.time:+.3f} s'
            else:
                next_line = 'no later engine Start/Stop line'

            fails.append(f'(a) {call.kind}Running ({call.why}) at {call.time - start_time:.3f} s '
                         f'[before: {call.before}] did not reach the engine ({next_line})')

    return session, redundant


def check_no_restart_after_stop(events, calls, session, start_time, fails):
    """Assertion (b): no "interruption ended" for a session whose last app call before it was a stop.

    "Before" is the engine log's line order (one file, written in order), not the clock: the probe's CALL and the
    engine's line often share a millisecond. (Seen: on 27.0 the app's foreground start reached the engine 1 ms after
    the restart; on 26.3 it never did.)
    """
    for index, event in enumerate(events):
        if event.kind != 'ended' or (session and event.session != session):
            continue

        earlier = [e for e in events[:index] if e.kind in ('start', 'stop') and e.session == event.session]
        if not earlier or earlier[-1].kind != 'stop':
            continue

        last_stop = earlier[-1]
        why = next((call.why for call in reversed(calls)
                    if call.kind == 'stop' and call.time <= last_stop.time + CLOCK_SLACK_S), '?')
        fails.append(f'(b) engine restarted session {event.session} at {event.time - start_time:.3f} s '
                     f'("interruption ended") although the app\'s last call was stopRunning ({why}) '
                     f'at {last_stop.time - start_time:.3f} s')


def check_final_stop(events, calls, session, start_time, fails):
    """Assertion (c): after the probe's final stopRunning, the engine has that session stopped."""
    if not calls or calls[-1].kind != 'stop':
        fails.append('(c) the probe\'s last call was not a stopRunning')
        return

    final_stop = calls[-1]
    mine = [e for e in events if e.kind in ('start', 'stop', 'ended') and (not session or e.session == session)]
    if not mine or mine[-1].kind != 'stop' or mine[-1].time < final_stop.time - CLOCK_SLACK_S:
        if mine:
            last = f'{ENGINE_LINE_NAMES[mine[-1].kind]} at {mine[-1].time - start_time:.3f} s'
        else:
            last = 'none'

        fails.append(f'(c) after the final stopRunning ({final_stop.why}) at {final_stop.time - start_time:.3f} s '
                     f'the session\'s last engine line is {last}, not that StopRunning')

    late = [e for e in mine if e.kind == 'ended' and e.time >= final_stop.time - CLOCK_SLACK_S]
    if late:
        fails.append(f'(c) "interruption ended" after the final stopRunning at {late[0].time - start_time:.3f} s')


def stopped_while_interrupted(events, session):
    """Tells whether an app stop reached the engine while it had the session interrupted.

    That is an engine StopRunning after an interrupt line with no "interruption ended" in between (engine log order).
    It also counts just before the interrupt line: the engine logs StopRunning on entry, then waits for the pump
    queue, where the interrupt may already be queued (appNotif's stop in didEnterBackground). A session whose stop
    had already run isn't running, so it isn't interrupted: the interrupt line proves the stop ran during it.
    """
    state = None
    for event in events:
        if session and event.session != session:
            continue

        if (event.kind, state) in (('stop', 'interrupt'), ('interrupt', 'stop')):
            return True

        state = event.kind if event.kind in ('interrupt', 'stop') else None

    return False


def main():
    run_dir, behaviour = sys.argv[1], sys.argv[2]
    app = timed_lines(read_lines(run_dir, 'app.log'), APP_LINE)
    engine = timed_lines(read_lines(run_dir, 'dylib.log'), ENGINE_LINE)
    if app:
        engine = [(time, line) for time, line in engine if time >= app[0][0] - EARLIER_PROCESS_S]

    missing = missing_setup(app, engine)
    if missing:
        print('FAIL setup: no ' + ', '.join(missing))
        sys.exit()

    calls = probe_calls(app)
    end_time = next(time for time, line in app if line.startswith('END |'))
    events = session_events(engine)
    start_time = app[0][0]
    fails = []

    runs = [event for event in events if event.kind in ('start', 'stop')]
    session, redundant = check_calls_reach_engine(calls, runs, end_time, start_time, fails)
    session = session or (runs[0].session if runs else None)
    check_no_restart_after_stop(events, calls, session, start_time, fails)
    check_final_stop(events, calls, session, start_time, fails)
    if behaviour in INTERRUPTED_STOP_BEHAVIOURS and not stopped_while_interrupted(events, session):
        fails.append('not exercised: no app stopRunning reached the engine while it had the session interrupted')

    expected_redundant = EXPECTED_REDUNDANT.get(behaviour, 0)
    if redundant > expected_redundant:
        fails.append(f'{redundant} redundant calls not forwarded (this behaviour makes {expected_redundant})')

    interrupts = sum(event.kind == "interrupt" for event in events)
    ended = sum(event.kind == "ended" for event in events)
    info = (f'calls={len(calls)} (redundant, not forwarded: {redundant}) engine start/stop={len(runs)} '
            f'interrupts={interrupts} ended={ended}')
    print(('FAIL ' + '; '.join(fails) + ' | ' + info) if fails else 'PASS ' + info)


if __name__ == '__main__':
    main()
