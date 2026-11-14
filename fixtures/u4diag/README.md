# u4diag (w4 e26, U4)

A copy of spike m11's test app for design §11 U4: eight extra shared schemes (`U4Base`, `U4ASan`, `U4TSan`, `U4UBSan`,
`U4Zombie`, `U4GMalloc`, `U4NoMTC`, `U4NoQueue`), each `M11Unit` plus one diagnostic in its Test and Run actions. The hosted
test (`UnitTests/HostedTests.swift`) and the app log which tool libraries and sanitizer runtimes are mapped, whether the
diagnostic is in effect (`__asan_init`/`__tsan_init`, `_NSZombie_*`, `MallocStackLogging`) and the camera result, to `$M11_OUT`.
`mark.c` builds `libu4mark.dylib`, a stand-in for a per-launch insert (Detox, idb). Usage and results:
measured internally, W4 e26.

Test plans (w4 e43, review r1 B1/M2): scheme `U4UIPlan` is `M11UI` with test plans instead of testables:
`M11UIASan.xctestplan` (default; one configuration, Address Sanitizer on in `defaultOptions`) and `M11UIMixed.xctestplan`
(configurations `ASan` and `TSan`). `xcodebuild build-for-testing -scheme U4UIPlan -testPlan M11UIMixed` writes an
`.xctestrun` with both configurations, each runner inserting its own sanitizer runtime. Results: measured internally, W4 e43.
