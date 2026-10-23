#!/usr/bin/env python3
"""Recompile the game's i960 code, its TGP program and the sound board's 68000
program to native C++ and build them. Same steps on Linux, macOS and Windows.

  recompile.py [--build-dir build] [--config Release]

Needs the user's ROM set at roms/daytona93.zip or roms/daytona93.7z
(git-ignored). Everything
derived from it (images, generated C++) goes under the build directory,
which is git-ignored: never commit it.
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def tool(build, config, name):
    """Path of a built tool: single-config generators put it in the build
    directory, Visual Studio in build/<config>/."""
    exe = name + (".exe" if os.name == "nt" else "")
    for p in (os.path.join(build, exe), os.path.join(build, config, exe)):
        if os.path.exists(p):
            return p
    sys.exit(f"recompile: {name} was not built in {build}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--config", default="Release")
    args = ap.parse_args()
    build = os.path.join(ROOT, args.build_dir)
    cache = os.path.join(build, "rom_cache", "daytona93")
    roms = [os.path.join(ROOT, "roms", "daytona93." + ext) for ext in ("zip", "7z")]
    roms = [r for r in roms if os.path.exists(r)]

    build_cmd = ["cmake", "--build", build, "--config", args.config]
    if not all(os.path.exists(os.path.join(cache, f)) for f in ("tgp_program.bin", "sound_program.bin", "pcm1.bin")):
        if not roms:
            sys.exit("recompile: put your ROM set at roms/daytona93.zip (or .7z) first")
        run(build_cmd + ["--target", "m2import"])
        run([tool(build, args.config, "m2import"), roms[0], cache])
    run(build_cmd + ["--target", "m2recomp", "m2tgprecomp", "m2sndrecomp"])

    gen = os.path.join(build, "gen", "daytona93")
    if os.path.isdir(gen):
        for f in os.listdir(gen):
            os.remove(os.path.join(gen, f))
    os.makedirs(gen, exist_ok=True)
    run([tool(build, args.config, "m2recomp"), os.path.join(cache, "program.bin"), gen,
         "--seeds", os.path.join("seeds", "daytona93.txt")])
    tgp = os.path.join(build, "gen", "daytona93_tgp")
    os.makedirs(tgp, exist_ok=True)
    run([tool(build, args.config, "m2tgprecomp"), os.path.join(cache, "tgp_program.bin"),
         os.path.join(tgp, "tgp_gen.cpp")])

    snd = os.path.join(build, "gen", "daytona93_snd")
    os.makedirs(snd, exist_ok=True)
    run([tool(build, args.config, "m2sndrecomp"), os.path.join(cache, "sound_program.bin"),
         os.path.join(snd, "snd_gen.cpp")])

    run(["cmake", "-S", ".", "-B", build])  # picks up the generated sources
    run(build_cmd + ["--parallel"])  # the game (daytona, m2run) and the check tools


if __name__ == "__main__":
    main()
