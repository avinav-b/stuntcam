# SPDX-License-Identifier: Apache-2.0
# activation/threads.sh — first camera calls from several threads at once.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.

# tsan_ours <app.log>: counts the ThreadSanitizer reports that may be the engine's, and prints three counts:
# "<with a libsimcam frame> <with an unrestored stack> <with no SUMMARY>". Each report counts once, under the first
# reason that applies.
# A report runs from its WARNING line to its SUMMARY line. A report that a new WARNING or the end of the log
# interrupts first was cut short, because the app was killed mid-report. It is counted by what its frames say, or
# else as cut short.
tsan_ours() {
  awk '
    function end_report() {
      if (!in_report) return
      if (simcam_frame) simcam_reports++
      else if (unrestored) unrestored_reports++
      else if (!summary) cut_reports++
      in_report = 0
    }
    /WARNING: ThreadSanitizer/ {
      end_report()
      in_report = 1
      simcam_frame = 0
      unrestored = 0
      summary = 0
      next
    }
    in_report && /libsimcam/ { simcam_frame = 1 }
    in_report && /\[failed to restore the stack\]/ { unrestored = 1 }
    in_report && /SUMMARY: ThreadSanitizer/ {
      summary = 1
      end_report()
    }
    END {
      end_report()
      print simcam_reports + 0, unrestored_reports + 0, cut_reports + 0
    }' $1
}

c_threads() {
  clear_sentinel $PLAIN_BUNDLE

  local launches=${THREADS_N:-100}
  local passed=0
  local no_done=0
  local failures=
  local tsan_all=0
  local tsan_engine=0
  local simcam_frames=0
  local unrestored=0
  local cut_short=0
  local i dir tsan_count engine_count kinds

  for i in {1..$launches}; do
    run plain th$i M5APP_ORDER=threads
    dir=$(run_dir plain th$i)
    # ThreadSanitizer reports come only from a TSan build of the app and the engine, which is the threads x100 half
    # of act-tsan. A normal build has none.
    # A report counts against the case, as possibly ours, if it has any of these:
    #   - a libsimcam frame;
    #   - a stack TSan could not restore ("[failed to restore the stack]"), since the other access may be ours;
    #   - no SUMMARY line, because the app was killed mid-report.
    # plain's own races are counted apart. They are on its M9 frame counter, with both stacks restored and no
    # libsimcam frame.
    tsan_count=$(count_matches 'WARNING: ThreadSanitizer' $dir/app.log)
    tsan_all=$((tsan_all + tsan_count))
    engine_count=0
    if (( tsan_count )); then
      cp $dir/app.log $SIMCAM_RESULTS/activation-$RUNTIME-$TAG-th$i-tsan.log
      kinds=$(tsan_ours $dir/app.log)
      local -a ours
      ours=(${=kinds})   # libsimcam frame, unrestored stack, cut short
      engine_count=$(( ours[1] + ours[2] + ours[3] ))
      simcam_frames=$((simcam_frames + ours[1]))
      unrestored=$((unrestored + ours[2]))
      cut_short=$((cut_short + ours[3]))
    fi
    tsan_engine=$((tsan_engine + engine_count))
    local threads=$(count_matches 'PASS order.threads ' $dir/app.log)
    local armed=$(count_matches 'LAZY armed by' $dir/dylib.log)
    if [[ $(count_matches DONE $dir/app.log) != 1 ]]; then
      no_done=$((no_done + 1))
      failures="$failures $i:nodone"
    elif (( engine_count )); then
      failures="$failures ${i}:tsan-engine=$engine_count"
    elif [[ $threads == 1 && $armed == 1 && $(pass_count $dir) == "$ORDER_RUN_CHECKS" ]]; then
      passed=$((passed + 1))
    else
      failures="$failures $i:$(grep -oE '(PASS|FAIL) order.threads[^|]*' $dir/app.log | cut -c1-60)"
    fi
    rm -rf $dir   # keep results small; a failing run's verdict is in the line
  done

  if (( passed == launches )); then
    ok "$launches/$launches: no deadlock, one armed-by line, all 3 threads saw cameras, 15/15;" \
      "ThreadSanitizer: $tsan_engine possibly ours ($simcam_frames libsimcam frame, $unrestored unrestored stack," \
      "$cut_short cut short), $tsan_all in all"
  else
    bad "$passed/$launches ok, $no_done no DONE, TSan $tsan_engine possibly ours ($simcam_frames libsimcam," \
      "$unrestored unrestored, $cut_short cut short) /" \
      "$tsan_all all;${failures:0:300}"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# thread_first_calls <run dir>: each thread's first-call ms, space-separated (a trailing space)
thread_first_calls() {
  grep -oE 'firstcall\.ms thread=[0-9] ms=-?[0-9]+' $1/app.log | grep -oE 'ms=-?[0-9]+$' | cut -d= -f2 | tr '\n' ' '
}

c_threads-slow() {
  clear_sentinel $PLAIN_BUNDLE

  local dir1 dir2 ms_500 ms_1500
  run plain ts1 M5APP_ORDER=threads SIMCAM_TEST_SLOW_ACTIVATE_MS=500
  dir1=$(run_dir plain ts1)
  ms_500=$(thread_first_calls $dir1)
  clear_sentinel $PLAIN_BUNDLE
  run plain ts2 M5APP_ORDER=threads SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  dir2=$(run_dir plain ts2)
  ms_1500=$(thread_first_calls $dir2)

  # With a 500 ms prepare, every thread waits for it (± 50 ms).
  # With a 1500 ms prepare, every thread times out at 1000 ms (± 100 ms).
  local ok_500=1
  local ok_1500=1
  local ms

  for ms in ${=ms_500}; do
    in_range $ms 450 550 || ok_500=0
  done

  for ms in ${=ms_1500}; do
    in_range $ms 900 1100 || ok_1500=0
  done

  local threads1=$(count_matches 'PASS order.threads ' $dir1/app.log)
  local stock2=$(count_matches 'PASS order.threads.stock' $dir2/app.log)

  if [[ $ok_500 == 1 && ${#${=ms_500}} == 3 && $threads1 == 1 && $ok_1500 == 1 && ${#${=ms_1500}} == 3 &&
    $stock2 == 1 ]]; then
    ok "500: all armed, per-thread ms $ms_500; 1500: each returned by t0+1.1 s ($ms_1500), all 3 saw stock"
  else
    bad "500: ms '$ms_500' threads=$threads1; 1500: ms '$ms_1500' stock=$stock2"
  fi

  clear_sentinel $PLAIN_BUNDLE
}
