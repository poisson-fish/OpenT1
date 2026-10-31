"""Regenerate committed GPU bytecode with the official DXC command line tool.

Usage: python compile.py /path/to/dxc
MSL sources mirror the short HLSL programs and are compiled by Metal at runtime.
Normal OpenT1 builds embed these artifacts and need no shader compiler or network.
"""

from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
for stage, profile in (("vert", "vs_6_0"), ("frag", "ps_6_0")):
    source = root / f"material.{stage}.hlsl"
    for extension, flags in (
        ("dxil", ["-Qstrip_debug", "-Qstrip_reflect"]),
        ("spv", ["-spirv", "-fspv-target-env=vulkan1.0"]),
    ):
        subprocess.run(
            [
                sys.argv[1],
                str(source),
                "-E",
                "main",
                "-T",
                profile,
                "-O3",
                "-WX",
                *flags,
                "-Fo",
                str(root / f"material.{stage}.{extension}"),
            ],
            check=True,
        )
