"""Create an isolated two-megabyte MBR/VBR test disk; contains no OS files."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mbr", type=Path)
    parser.add_argument("vbr", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    mbr, vbr = args.mbr.read_bytes(), args.vbr.read_bytes()
    if len(mbr) != 512 or len(vbr) != 512:
        raise ValueError("both boot sectors must be exactly 512 bytes")
    image = bytearray(2 * 1024 * 1024)
    image[:512] = mbr
    image[512:1024] = vbr
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"Wrote isolated test disk: {args.output}")


if __name__ == "__main__":
    main()
