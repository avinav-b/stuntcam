<!-- SPDX-License-Identifier: Apache-2.0 -->
# Upgrading

## From 1.1.0

stuntcam 1.2.0 renamed the engine's environment variables to plain `SIMCAM_*` names. Six of them are camera settings
that `stuntcam up` writes into a simulator's environment, so a simulator armed by 1.1.0 still carries their old names:
in its boot environment (boot mode, until it shuts down) or its launchd environment (launchd mode, until
`stuntcam down`, a re-arm or a shutdown). A newer build can start running there without a re-arm: `stuntcam up` on a
sim armed in boot mode this boot only points it at the new build (no reboot), and `stuntcam swap` changes the build
and nothing else.

| new name | 1.1.0 name |
|---|---|
| `SIMCAM_FIG_SOURCES` | `M2_FAKE` |
| `SIMCAM_FIG_SESSION` | `M2_SESSION_HOOK` |
| `SIMCAM_OFF_KEY_LOG` | `M2_NOLOGDICT` |
| `SIMCAM_PROFILE` | `M2_PLIST` |
| `SIMCAM_FIXTURE_IMAGE` | `M5_IMAGE` |
| `SIMCAM_ENGINE_LOG` | `M5_LOG` |

What keeps working, and why:

- The engine reads each of the six under its new name first and its 1.1.0 name second (`ENV_LEGACY_*` and
  `EnvConfig` in `engine/src/core/env.inc`), so a newer engine on a sim armed by 1.1.0 still has its camera. No
  other variable has a fallback: a renamed off switch, test fault, tuning or diagnostic variable works only under its
  new name (`engine/src/core/env.inc` lists every variable the engine reads).
- The CLI sets both names for the six (`LegacyNames` in `cli/internal/arm`): the new name, then the old one with the
  same value. A 1.1.0 engine that a newer CLI points a sim at (`stuntcam swap` to a 1.1.0 build, `up --build`, the
  login service running the build it pinned at `service install`) still finds its names. `stuntcam env` prints
  both, `exec` exports both, and arm records list both.
- `stuntcam down` unsets exactly the names an arm record lists: the old six for a 1.1.0 record, both sets for a new
  one. A launchd re-arm (`stuntcam up`) over a 1.1.0 arm sets every new and every old name and unsets none of them
  on the way.
- The runner hook (`engine/runnerhook`) forwards the six under both names on a launchd-armed sim.

The old names go from the engine and the CLI together, in 2.0 at the earliest.

### Other changes in 1.2.0

- `stuntcam exec` copies the caller's exported `SIMCAM_*` variables to the test runner (as `TEST_RUNNER_*`), except
  its own and a simulator's paths and camera settings. (The runner hook, on a launchd-armed sim, forwards the
  runner's `SIMCAM_*` to the apps a test launches, the six camera settings included, under both names: those are
  the sim's own.) Since the off switches, test faults, tuning and diagnostic variables now start with `SIMCAM_`,
  one exported in the shell that runs `exec` reaches the test runner; under their 1.1.0 names they never did. The
  six camera settings are still not taken from the caller's shell: each destination gets its own.
- `M2_PREFLIGHT_BREAK` and `M2_PF_FAKEBREAK`, two aliases of `SIMCAM_PF_BREAK`, are no longer read: use
  `SIMCAM_PF_BREAK`.
- `scripts/activation.sh`: the input `PRB=1` is now `MOTION_SUITE=1` (the old name is not read). Case names are
  unchanged.
- Engine log lines no longer carry internal run codes, and the scripts that match them changed with them; for
  example the photo-stop mismatch line is now `MISMATCH: photo stop`.
- The Objective-C classes the engine registers in an app's process are named `Simcam*` (an app printing the class of
  a CoreMotion stand-in sees `SimcamAttitude`). Archives are unaffected: the stand-in encodes as `CMAttitude`.
