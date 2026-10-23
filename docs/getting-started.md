# Getting started

From nothing to playing, on Windows, macOS or Linux. Setup installs what it
needs, downloads the libraries, recompiles the game from your ROM set and
builds it. You run one command; it takes a while the first time.

## What you need

1. **Your own `daytona93` ROM set.** This is *Daytona USA Deluxe '93*, the
   set MAME calls `daytona93`, as a `.zip` or `.7z`. No game data comes
   with this project: the game is built from your copy.

   It must be that exact set. Other Daytona USA sets (`daytona`,
   `daytonas`, `daytonat` and so on) have different program ROMs and are
   rejected, even if you rename the file. You can tell `daytona93` apart:
   it contains `epr-16530a.12`, `epr-16531a.13`, `epr-16534a.6` and
   `epr-16535a.7`. A set that has `epr-16722a.12` instead is a different
   version.

2. **A 64-bit computer** running Windows 10 or 11, macOS, or a common Linux
   distribution (Debian/Ubuntu, Fedora, Arch or openSUSE), with an internet
   connection for the first setup and a few GB of free disk space.

3. **Git**, to download the project. Setup installs everything else.

   - Windows: `winget install Git.Git` in PowerShell, or
     <https://git-scm.com>.
   - macOS: type `git` in Terminal and accept the offer to install the
     command line tools.
   - Linux: `sudo apt install git` (or your distribution's equivalent).

## 1. Download the project

Open a terminal (PowerShell on Windows, Terminal on macOS) and run:

    git clone https://github.com/alphanu1/daytona-arcade-recomp.git
    cd daytona-arcade-recomp

Stay in this folder for every command below.

## 2. Put your ROM set in place

Make a folder called `roms` inside the project and copy your set into it,
named **exactly** `daytona93.zip` (or `daytona93.7z`):

    daytona-arcade-recomp/
        roms/
            daytona93.zip

macOS and Linux:

    mkdir -p roms
    cp /path/to/your/daytona93.zip roms/daytona93.zip

Windows (PowerShell):

    mkdir roms
    copy C:\path\to\your\daytona93.zip roms\daytona93.zip

Do this before setup. Without it, setup builds only the tools and there is
no game to run. The `roms` folder is never uploaded anywhere: git ignores
it.

## 3. Run setup

**macOS and Linux:**

    ./setup.sh

The name ends in `.sh`: `./setup` on its own is "no such file".

- macOS: needs Homebrew (<https://brew.sh>); setup tells you how to install
  it if it is missing. The first time, macOS may open a window offering the
  Xcode command line tools: install them, then run `./setup.sh` again.
- Linux: setup installs packages with `sudo`, so it asks for your password.

**Windows** (PowerShell):

    powershell -ExecutionPolicy Bypass -File setup.ps1

It installs Git, CMake, Ninja, Python and the Visual Studio 2022 Build
Tools with winget. The Build Tools are a large download, and Windows may
ask for permission. If it says Python "is not on PATH yet", close
PowerShell, open a new one and run the same command again.

Setup prints each step. It is done when you see:

    == Done

    The game is built. Start it with:

        build/daytona

If it stops before that, see [Troubleshooting](#troubleshooting).

## 4. Play

macOS and Linux:

    build/daytona

Windows:

    build\Release\daytona.exe

(Use the exact path setup printed: it is `build\daytona.exe` if setup
used Ninja.)

A launcher opens first. On the **Game** tab, click **Browse...**, choose
the same `roms/daytona93.zip`, and wait for the line under it to say "All
30 files verified." Then click **Start**. The launcher remembers the file
next time. **Controls** sets your keys and gamepad. In the game, **Esc**
brings the launcher back.

Default keys: arrows to steer, accelerate and brake; 5 inserts a coin,
Enter is start; A S D F are the view buttons; 1-4 or Q/W change gear. The
full table is in the [README](../README.md#playing).

To skip the launcher:

    build/daytona --rom roms/daytona93.zip --autostart

## Updating

To get a newer version, from the project folder:

    git pull
    ./setup.sh

(`setup.ps1` on Windows.) Always run setup after pulling: updates can
change how the game is recompiled, and setup redoes that from your ROM
set. It is much quicker the second time.

## Troubleshooting

**`zsh: no such file or directory: ./setup`**
The script is `./setup.sh`.

**`No ROM set: the tools are built, the game is not`**, or
**`build/daytona: no such file or directory`**
Setup did not find `roms/daytona93.zip` or `roms/daytona93.7z`. Check the
folder is called `roms`, is inside the project folder, and the file has
exactly that name (not `daytona.zip`, not `daytona93.zip.zip`: Windows can
hide the extension). Setup lists any archives it found in `roms/`. Fix it
and run setup again.

**`m2import: missing epr-16530a.12`** (or another file), then
**`your ROM set was rejected`**
The file is not the `daytona93` set, or is incomplete. See
[What you need](#what-you-need). Renaming a different set does not help:
its program is different.

**Start is greyed out in the launcher**
The line under the ROM path says why. If it says files are missing or
wrong, the file you chose is not the `daytona93` set. If it cannot open the
file, click **Browse...** and choose it again.

**The game closes straight away, mentioning `SDL_CreateGPUDevice`**
The graphics option in the launcher is set to one your computer lacks
(Vulkan on a Mac, for instance). Current versions fall back to automatic;
on an older one, set **Graphics API** to **Automatic**, or delete the
settings file below.

**`no recompiled code at 00xxxxxx: add it to the seeds`**
The game reached code this version does not have yet. Run `git pull` and
setup again; if it still happens, open an issue on GitHub with the address
and what you were doing in the game.

**Starting again from scratch**
Delete the `build` folder and run setup again. Your ROM set in `roms/` is
kept. Launcher settings, the game's settings EEPROM and backup RAM are in:

- macOS: `~/Library/Application Support/daytona-recomp/daytona93/`
- Windows: `%APPDATA%\daytona-recomp\daytona93\`
- Linux: `~/.local/share/daytona-recomp/daytona93/`

Delete that folder to reset them.

## Status

The game builds and plays on macOS (Apple silicon, Metal). The Windows and
Linux builds use the same code and setup, but have had less testing.
Problems on any platform are worth an issue on GitHub, with the full
output of setup.
