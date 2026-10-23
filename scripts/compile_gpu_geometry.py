"""Compile the presentation compute shader; no game assets are used."""
from pathlib import Path
import argparse
import subprocess
import struct

parser = argparse.ArgumentParser()
parser.add_argument('glslang', help='Path to glslangValidator')
parser.add_argument('--shader', type=Path, help='Private experimental GLSL source')
parser.add_argument('--output-dir', type=Path, help='Private compiled shader output directory')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
if bool(args.shader) != bool(args.output_dir):
    parser.error('Private shader and output directory must be supplied together')
binary = args.output_dir / 'gpu_geometry.spv' if args.output_dir else root / 'build/gpu_geometry.spv'
binary.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([args.glslang, '-V', str(args.shader or root / 'src/app/gpu_geometry.comp'), '-o', str(binary)], check=True)
data = binary.read_bytes()
words = struct.unpack('<' + 'I' * (len(data) // 4), data)
lines = [','.join(f'0x{word:08x}u' for word in words[i:i+8]) for i in range(0, len(words), 8)]
(args.output_dir / 'gpu_geometry_spirv.h' if args.output_dir else root / 'src/app/gpu_geometry_spirv.h').write_text(
    '#pragma once\n// Generated from gpu_geometry.comp; no game assets.\n'
    'inline constexpr unsigned int gpu_geometry_spirv[] = {\n' + ',\n'.join(lines) + '\n};\n')
