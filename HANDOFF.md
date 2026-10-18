# Publication handoff

## State: 18 October 2026

The public repository has been created. No project source snapshot or executable release has been published yet.

Source: upstream alphanu1/daytona-arcade-recomp, base commit 398eb4bb77be3b14a3636f8f8661b4e4a2b698f0, with substantial local Windows, Android and console work.

## Verified checks

- Public visibility and owner were verified through GitHub.
- The Android APK archive contains assets/roms/daytona93.zip.
- Desktop and console build definitions link locally generated game code.
- The source drive refused new file writes and failed reading at least one current console source file. Publication must not invent or omit required source silently.
- No game package, ROM or generated game code has been uploaded.

## Next work

1. Restore reliable read and write access to the project drive.
2. Prepare and inspect the complete current project-owned source snapshot, preserving dependency notices and upstream attribution.
3. Exclude private paths/configuration, keys, caches, generated game code and assets.
4. Validate the snapshot and publish it, then verify the remote files.
5. Prepare distributable build tools or local-build instructions; existing private game binaries cannot be uploaded under the project's rules.

## Do not repeat

Do not upload an entire working directory or existing private APK/console ZIP. Removing external game-data files does not remove game-derived machine code from a compiled executable. Do not claim the New 3DS reaches 30 FPS; current clean emulator evidence is approximately 15 FPS.
