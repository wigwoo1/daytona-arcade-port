# Daytona Arcade Port

A source snapshot based on alphanu1/daytona-arcade-recomp, base commit 398eb4bb77be3b14a3636f8f8661b4e4a2b698f0.

This repository contains desktop, Android and experimental New Nintendo 3DS source. No game ROM, ROM-generated game code, extracted game assets, signing keys, or compiled game executable is included. You must supply your own legally obtained, supported Daytona ROM set and build locally.

## Platforms

- Desktop: follow docs/getting-started.md. Setup fetches pinned dependencies, recompiles game code from your ROM, builds and runs available tests.
- Android: ARM64 app source is under android/. It uses locally generated game code and Android assets. Android builds need a compatible SDK/NDK, installed Gradle 8.12, your own ROM, and a local signing key. The Gradle wrapper and APK are not included.
- New Nintendo 3DS: experimental homebrew source is included under 3ds/ and src/app/. It is not a validated playable release. Device SDKs and ROM-generated code/assets must be supplied locally. ROM-derived packed asset files are intentionally omitted.

Build outputs, local SDKs, generated game code, logs and captures are not part of this source snapshot. See HANDOFF.md for publication scope and validation limits, and THIRD_PARTY.md for dependency notices.

## Licensing

This repository does not add a blanket license for local modifications. The upstream project and each third-party component retain their own notices; see the upstream repository and THIRD_PARTY.md.