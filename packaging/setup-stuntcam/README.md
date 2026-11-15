<!-- SPDX-License-Identifier: Apache-2.0 -->
# setup-stuntcam

A GitHub Action that installs stuntcam and arms a simulator for UI tests. Not published yet.

```yaml
- uses: avinav-b/setup-stuntcam@v1
  id: stuntcam
  with: { grant: com.example.app }
- run: stuntcam exec -- xcodebuild test -scheme AppUITests -destination "id=${{ steps.stuntcam.outputs.udid }}"
```

Inputs: `version`, `tarball`, `up` (`false` installs only), `udid` (default: the booted simulator, else a new
`stuntcam-ci` iPhone), `mode`, `grant`, `args`. Outputs: `path`, `version`, `udid`.

`action.template.yml` is the source; `scripts/package.sh` writes the action to `build/package/setup-stuntcam/`.
