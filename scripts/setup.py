#!/usr/bin/env python3
"""Fetch everything needed to build daytona-arcade-recomp and build it. The
same steps on Linux, macOS and Windows; run ./setup.sh (Linux, macOS) or
setup.ps1 (Windows) first to install the toolchain, or run this directly if
you already have one.

  setup.py [--test-extras] [--with-mame] [--build-mame] [--no-build]

Always:
  - checks the toolchain: git, CMake >= 3.20, a C++20 compiler (Ninja used
    when present; Visual Studio's generator on Windows);
  - fetches Berkeley SoftFloat 3e, SDL 3, ymfm, the LZMA SDK and Dear ImGui
    at their pinned commits into extern/ (all built from source with the
    project, statically);
  - configures and builds the tools and tests, runs the tests;
  - if your ROM set is at roms/daytona93.zip (or .7z): imports it, recompiles the
    game's code to native C++ and builds it (all under build/, git-ignored).
Options:
  --test-extras  pip packages and modules for the optional tests (lupa for the
                 Lua plugin tests, pypcode + the Ghidra i960 module for the
                 second decode oracle).
  --with-mame    MAME source at its pinned commit, patched (the disassembler
                 oracle test; needed before --build-mame).
  --build-mame   build the patched, Model-2-only MAME that records the traces
                 the lockstep checks compare against (Linux and macOS; tens of
                 minutes). Only for validation: the game never uses MAME.

Nothing fetched here is committed (extern/ is git-ignored), and no ROM data
ever leaves roms/ and build/.
"""

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTERN = os.path.join(ROOT, "extern")
WINDOWS = os.name == "nt"

# Pinned third-party sources (THIRD_PARTY.md).
SOFTFLOAT = ("https://github.com/ucb-bar/berkeley-softfloat-3.git", "a0c6494cdc11865811dec815d5c0049fba9d82a8")
SDL3 = ("https://github.com/libsdl-org/SDL.git", "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c")  # release-3.4.16
IMGUI = ("https://github.com/ocornut/imgui.git", "f1cc2ae15e53a861a874c3034aae6798fde194ab")  # v1.92.9b
YMFM = ("https://github.com/aaronsgiles/ymfm.git", "81aec25ccbb98f4873a255f7551ac4dadac59b4a")  # YM3438 (sound board FM)
LZMA_SDK = ("https://github.com/ip7z/7zip.git", "0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17")  # 7-Zip 26.03 (C/: LZMA SDK)
GHIDRA_I960 = ("https://github.com/mumbel/ghidra_i960.git", "727ef7872c5b1cd6ceb5a81f5e474d1ced92945c")
MAME_COMMIT = "dddd73680656e355bb2b5beecab1167c9f07bf81"


def say(msg):
    print(f"\n== {msg}", flush=True)


def run(cmd, cwd=ROOT, check=True):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd, check=check)


def need(tool, hint):
    path = shutil.which(tool)
    if not path:
        sys.exit(f"setup: {tool} not found. {hint}")
    return path


def check_toolchain():
    say("Checking the toolchain")
    need("git", "Install Git (setup.sh / setup.ps1 do this).")
    need("cmake", "Install CMake 3.20 or newer (setup.sh / setup.ps1 do this).")
    out = subprocess.run(["cmake", "--version"], capture_output=True, text=True).stdout
    m = re.search(r"(\d+)\.(\d+)", out)
    if not m or (int(m.group(1)), int(m.group(2))) < (3, 20):
        sys.exit(f"setup: CMake 3.20 or newer is needed, found {out.splitlines()[0] if out else 'none'}")
    if not WINDOWS and not (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")):
        sys.exit("setup: no C++ compiler found (setup.sh installs one)")
    print(f"{platform.system()} {platform.machine()}, Python {platform.python_version()}, {out.splitlines()[0]}")


def fetch(url, commit, dest):
    dest = os.path.join(EXTERN, dest)
    if not os.path.isdir(os.path.join(dest, ".git")):
        run(["git", "clone", "--no-checkout", url, dest])
    run(["git", "-C", dest, "fetch", "--depth", "1", "origin", commit], check=False)
    run(["git", "-C", dest, "checkout", "--detach", commit])


def fetch_mame(full):
    """The pinned MAME with our oracle patches. Sparse (the files the tests
    read) unless the full tree is needed to build it."""
    dest = os.path.join(EXTERN, "mame")
    if not os.path.isdir(os.path.join(dest, ".git")):
        run(["git", "clone", "--filter=blob:none", "--no-checkout", "https://github.com/mamedev/mame.git", dest])
    if full:
        run(["git", "-C", dest, "sparse-checkout", "disable"], check=False)
    else:
        run(["git", "-C", dest, "sparse-checkout", "set", "--no-cone",
             "/src/devices/cpu/i960/", "/src/devices/cpu/mb86233/",
             "/src/devices/cpu/m68000/m68000.cpp", "/src/devices/cpu/m68000/m68000.h",
             "/src/mame/sega/model2.cpp", "/src/mame/sega/model2.h",
             "/src/mame/sega/model2_v.cpp", "/src/mame/sega/model2_m.cpp",
             "/src/mame/shared/segam1audio.cpp", "/src/mame/shared/segam1audio.h"])
    run(["git", "-C", dest, "fetch", "--depth", "1", "origin", MAME_COMMIT])
    run(["git", "-C", dest, "checkout", "--detach", MAME_COMMIT])
    patches = os.path.join(ROOT, "patches", "mame")
    for p in sorted(os.listdir(patches)):
        if p.endswith(".patch"):
            path = os.path.join(patches, p)
            if subprocess.run(["git", "-C", dest, "apply", "--check", path], capture_output=True).returncode == 0:
                run(["git", "-C", dest, "apply", path])


def configure_and_build(build):
    say("Configuring and building")
    cmd = ["cmake", "-S", ".", "-B", build, "-DCMAKE_BUILD_TYPE=Release"]
    if WINDOWS and not (shutil.which("cl") and shutil.which("ninja")):
        cmd += ["-A", "x64"]  # Visual Studio generator: finds MSVC without a developer prompt
    elif shutil.which("ninja"):
        cmd += ["-G", "Ninja"]
    if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
        run(cmd)
    else:
        run(["cmake", "-S", ".", "-B", build])
    run(["cmake", "--build", build, "--config", "Release", "--parallel"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--test-extras", action="store_true")
    ap.add_argument("--with-mame", action="store_true")
    ap.add_argument("--build-mame", action="store_true")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--build-dir", default="build")
    args = ap.parse_args()
    build = os.path.join(ROOT, args.build_dir)

    check_toolchain()
    os.makedirs(EXTERN, exist_ok=True)

    say("Fetching SoftFloat 3e (extF80 reference for i960 FP)")
    fetch(*SOFTFLOAT, "softfloat")

    say("Fetching SDL 3.4.16 (window, input, SDL_GPU: Vulkan / Direct3D 12 / Metal)")
    fetch(*SDL3, "sdl3")

    say("Fetching ymfm (the sound board's YM3438)")
    fetch(*YMFM, "ymfm")

    say("Fetching the 7-Zip LZMA SDK 26.03 (7z ROM sets)")
    fetch(*LZMA_SDK, "lzma")

    say("Fetching Dear ImGui 1.92.9b (launcher interface)")
    fetch(*IMGUI, "imgui")

    if args.test_extras:
        say("Optional test extras")
        run([sys.executable, "-m", "pip", "install", "--user", "lupa", "pypcode"], check=False)
        fetch(*GHIDRA_I960, "ghidra_i960")

    if args.with_mame or args.build_mame:
        say("Fetching MAME (validation oracle only) and applying our patches")
        fetch_mame(full=args.build_mame)

    if args.build_mame:
        if WINDOWS:
            print("setup: building MAME on Windows needs MAME's MSYS2 toolchain; see "
                  "https://docs.mamedev.org/initialsetup/compilingmame.html. Skipped.")
        else:
            say("Building the Model-2-only MAME (this takes a while)")
            run(["sh", os.path.join("scripts", "build_mame.sh")])

    if args.no_build:
        return
    configure_and_build(build)

    game = None
    if any(os.path.exists(os.path.join(ROOT, "roms", "daytona93." + e)) for e in ("zip", "7z")):
        say("Recompiling the game to native code (your ROM set, kept in build/)")
        r = run([sys.executable, os.path.join("scripts", "recompile.py"), "--build-dir", args.build_dir], check=False)
        if r.returncode:
            sys.exit(ROM_REJECTED)
        game = next((p for p in (os.path.join(build, "daytona" + EXE), os.path.join(build, "Release", "daytona" + EXE))
                     if os.path.exists(p)), None)
    else:
        say("No ROM set: the tools are built, the game is not")
        print(no_rom_help())

    say("Running the tests")
    run(["ctest", "--test-dir", build, "-C", "Release", "--output-on-failure"], check=False)
    say("Done")
    if game:
        print("\nThe game is built. Start it with:\n\n    " + os.path.relpath(game, ROOT) + "\n")


EXE = ".exe" if WINDOWS else ""

ROM_REJECTED = """
setup: your ROM set was rejected (the line starting "m2import:" above names
the first file that is missing or wrong), so the game was not built.

This project needs the daytona93 set: Daytona USA Deluxe '93, as MAME names
it. Other Daytona sets (daytona, daytonas, daytonat, ...) have different
program ROMs and cannot be used, whatever the file is called. A daytona93 set
contains epr-16530a.12, epr-16531a.13, epr-16534a.6 and epr-16535a.7.

Put the right set at roms/daytona93.zip (or .7z) and run setup again.
See docs/getting-started.md, "Troubleshooting".
"""


def no_rom_help():
    roms = os.path.join(ROOT, "roms")
    found = sorted(f for f in os.listdir(roms) if f.lower().endswith((".zip", ".7z"))) if os.path.isdir(roms) else []
    lines = ["To build the game, put your own daytona93 ROM set (Daytona USA Deluxe '93)",
             "at roms/daytona93.zip or roms/daytona93.7z, exactly that name, and run setup again."]
    if found:
        lines += ["", "Found in roms/, but not under that name: " + ", ".join(found),
                  "If one of these is the daytona93 set, rename it to daytona93.zip (or .7z)."]
    return "\n".join(lines)


if __name__ == "__main__":
    main()
