#!/usr/bin/env python3
"""Render device screens through the SDL preview and export PNGs for docs.

Usage: python3 sim/render_docs.py [--scale 3] [--out docs/images/screens]

Builds the native-sim target, dumps every scenario as PPM, and converts the
frames to nearest-neighbour scaled PNGs so pixels stay crisp.
"""
from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scale", type=int, default=3)
    parser.add_argument("--out", default=str(ROOT.parent / "docs" / "images" / "screens"))
    parser.add_argument("--only", nargs="*", help="scenario names to export")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--sheet", help="also write a labelled contact sheet PNG")
    args = parser.parse_args()

    pio = os.environ.get("PIO", "pio")
    if not args.no_build:
        subprocess.run([pio, "run", "-e", "native-sim"], cwd=ROOT, check=True)
    program = ROOT / ".pio" / "build" / "native-sim" / "program"

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cardputer-shots-") as temp:
        subprocess.run([str(program), "--shots", temp], check=True)
        frames = sorted(glob.glob(f"{temp}/*.ppm"))
        written = []
        for frame in frames:
            name = Path(frame).stem
            if args.only and name not in args.only:
                continue
            image = Image.open(frame).convert("RGB")
            size = (image.width * args.scale, image.height * args.scale)
            image.resize(size, Image.NEAREST).save(out / f"{name}.png", optimize=True)
            written.append(name)
        if args.sheet:
            from PIL import ImageDraw

            cols = 4
            rows = (len(frames) + cols - 1) // cols
            sheet = Image.new("RGB", (cols * 250, rows * 160), (24, 26, 32))
            draw = ImageDraw.Draw(sheet)
            for index, frame in enumerate(frames):
                x, y = (index % cols) * 250 + 5, (index // cols) * 160 + 5
                sheet.paste(Image.open(frame).convert("RGB"), (x, y))
                draw.text((x, y + 138), Path(frame).stem, fill=(200, 200, 200))
            sheet.save(args.sheet)
    print(f"wrote {len(written)} screens to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
