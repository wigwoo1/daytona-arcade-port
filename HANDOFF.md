# Publication handoff

## State: 23 October 2026

This public repository now contains a reviewed, source-only snapshot based on upstream alphanu1/daytona-arcade-recomp commit 398eb4bb77be3b14a3636f8f8661b4e4a2b698f0. The snapshot includes desktop source, Android app source, experimental New Nintendo 3DS code, build instructions, third-party notices, and source tests.

This is not a playable release. No game executable, ROM, ROM-generated machine code, extracted game asset pack, APK, 3DSX/ELF, N64 ROM image, local SDK, signing key, capture, or private test log was published. The Android and 3DS builds require locally generated game code/assets from a supported ROM. The packed 3DS asset C files were excluded because they contain ROM-derived content; the N64 experiment and its ROM-derived data were also excluded.

The local development handoff, machine-specific editor settings, test-capture helpers with private paths, and per-run logs were not copied. This handoff records only public publication scope.

## Validation

- 186 allowlisted source/document/test files were reviewed and copied; the exact file list and SHA-256 hashes are in the local Y: publication audit.
- Desktop/Android/3DS binaries were not published.
- No build or test suite was run as part of this source publication pass. Existing private build results are not a substitute for a clean public build.
- The project has no blanket license for local changes. Upstream and third-party notices remain attributed separately.