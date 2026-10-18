# Daytona Arcade Port

Native port development based on [alphanu1/daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp).

This public repository has been created for the Windows, Android and New Nintendo 3DS project. **The current development source has not yet been uploaded.** The project drive became unreadable for some source files and refused new file writes during publication on 18 October 2026. Daily publication checks are scheduled in the maintainer's local environment.

## Current development status

- Windows: a private playable build exists, with controller bindings, configurable resolution and presentation rate, widescreen, draw distance, texture detail and LOD options.
- Android: a private ARM64 APK exists with controller support and a toggleable touch overlay.
- New Nintendo 3DS: experimental GPU rendering and stereo support; the latest clean emulator comparisons are approximately 15 FPS. The 30 FPS goal remains unmet. Emulator results do not establish performance on a physical console.

These describe local development results, not publicly downloadable builds or independent verification of every feature.

## Game data and builds

The project's publication rules exclude game ROMs, generated game code and extracted game assets. Existing private game executables link code generated from the ROM; the self-contained APK also includes a ROM ZIP. Those packages have not been uploaded.

A public release needs source and local-build instructions, or separately verified tools/packages that contain no game-derived code or assets. Simply removing the ROM ZIP from an existing game package is insufficient.

## Attribution and licensing

The source project is [alphanu1/daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp), local base commit `398eb4bb77be3b14a3636f8f8661b4e4a2b698f0`. Upstream attribution and third-party notices must be preserved when source publication resumes. The upstream project has not selected an overall source license; this repository does not assign one to upstream code.

This project is not affiliated with or endorsed by Sega. Daytona USA is a Sega trademark.

## Updates

A daily local task checks for publishable changes at 11:00 AM America/New_York. It requires the maintainer's computer, project drive and GitHub connection to be available. No upload is claimed until the remote commit or release is verified.
