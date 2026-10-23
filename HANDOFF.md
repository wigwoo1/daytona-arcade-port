# Publication handoff

## State: 23 October 2026

The first reviewed, source-only snapshot was published at commit dfe754a5836e4b12025123f0fbc3175fd69f938e. It is based on upstream alphanu1/daytona-arcade-recomp commit 398eb4bb77be3b14a3636f8f8661b4e4a2b698f0.

The snapshot includes desktop source, Android app source, experimental New Nintendo 3DS code, build documentation, third-party notices and source tests. It is not a playable release.

No game executable, ROM, ROM-generated machine code, extracted game asset pack, APK, 3DSX/ELF, N64 ROM image, local SDK, signing key, capture or private test log was published. Android and 3DS builds require locally generated game code/assets from a supported ROM. ROM-derived packed 3DS asset C files were excluded. The unrelated N64 experiment and its ROM-derived data were also excluded.

Machine-specific editor settings, capture/trace helpers with private paths, and the local development handoff were not copied.

## Validation

- The source-only snapshot contains 186 reviewed files totaling 1,648,524 bytes. Local and remote raw Git blob hashes matched for every file.
- The public GitHub tree was checked for excluded binaries and ROM-derived asset paths; none were present.
- No build or test suite was run for this publication. Existing local build results do not establish that a clean public build succeeds.
- No overall license is declared for local changes. Upstream and third-party notices remain separate.