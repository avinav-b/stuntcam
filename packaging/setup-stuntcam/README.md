<!-- SPDX-License-Identifier: Apache-2.0 -->
# setup-stuntcam (GitHub Action, unpublished)

Design §10 (CI): "build, `setup-stuntcam`, `exec`, canary". A composite action that installs stuntcam from npm into
`$RUNNER_TEMP` (no global install, nothing added to the project's `package.json`), puts it on `PATH`, restores the canary
passes cached for this stuntcam build and the runner's runtimes, runs `stuntcam up`, and saves the plain passes under a key
that is new on every run (`actions/cache/restore` + `save`; fails and short-lived retried passes are never cached).

`action.template.yml` is the source: `scripts/package.sh` writes the action, with the `version` default taken from the
`VERSION` file, to `build/package/setup-stuntcam/action.yml`; that generated directory is what gets published.

```yaml
jobs:
  ui-tests:
    runs-on: macos-26            # Apple silicon, Xcode 27
    steps:
      - uses: actions/checkout@v4
      - uses: avinav-b/setup-stuntcam@v1   # TODO: not published; needs a public repo + tag
        id: stuntcam                            # no udid: creates "stuntcam-ci" and boots it armed (boot mode)
        with: { grant: com.example.app }
      - run: stuntcam exec -- xcodebuild test -scheme AppUITests -destination "id=${{ steps.stuntcam.outputs.udid }}"
```

Inputs: `version` (npm, default the current release), `tarball` (a local `.tgz` instead), `up` (`false` = install only),
`udid` (default: the only booted iOS simulator, else a `stuntcam-ci` iPhone on the newest iOS runtime, created if
needed; several booted is an error asking for one), `mode`, `grant` (bundle ids), `args` (split like a shell: quotes
group words, nothing is expanded). Outputs: `path`, `version`, `udid`.
Never run on GitHub (no public repo yet); its install and `up` steps were run by hand on a Mac against the local tarball
(measured internally, W4 e51).
