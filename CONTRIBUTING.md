# Contributing

Issues and pull requests are welcome. For a bug, the issue template asks for `stuntcam doctor --json` and
`stuntcam status --json`; [docs/SUPPORT.md](docs/SUPPORT.md) says what's verified.

Before you push:

```sh
git config core.hooksPath tools/hooks   # once per clone: runs tools/privacy-check.sh before each push
sh tools/spdx-check.sh                  # new source files start with an SPDX-License-Identifier line
(cd cli && go test ./...)               # the CLI's tests; no simulator needed
```

CI runs the same checks on every pull request (`.github/workflows/checks.yml`). The engine and simulator tests need an
Apple silicon Mac with Xcode 27: `./build.sh`, then the scripts in `scripts/` (each prints its usage).
