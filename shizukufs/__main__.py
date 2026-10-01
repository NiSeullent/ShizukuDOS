"""Host CLI for building and checking ShizukuFS v0 disk images."""

from __future__ import annotations

import argparse
import json
import sys

from .format import CorruptImage, Image, NoSpace


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m shizukufs")
    commands = parser.add_subparsers(dest="command", required=True)

    create = commands.add_parser("create", help="create an empty v0 image")
    create.add_argument("image")
    create.add_argument("--blocks", type=int, default=256,
                        help="4096-byte block count (default: 256)")

    verify = commands.add_parser("verify", help="check every inode, chain, CRC, and bitmap")
    verify.add_argument("image")

    listing = commands.add_parser("ls", help="list a directory")
    listing.add_argument("image")
    listing.add_argument("path", nargs="?", default="/")

    mkdir = commands.add_parser("mkdir", help="create a directory")
    mkdir.add_argument("image")
    mkdir.add_argument("path")

    put = commands.add_parser("put", help="store or replace a file")
    put.add_argument("image")
    put.add_argument("source")
    put.add_argument("path")

    get = commands.add_parser("get", help="extract a file; destination must not exist")
    get.add_argument("image")
    get.add_argument("path")
    get.add_argument("destination")

    remove = commands.add_parser("rm", help="remove a file or empty directory")
    remove.add_argument("image")
    remove.add_argument("path")

    args = parser.parse_args(argv)
    try:
        if args.command == "create":
            with Image.create(args.image, args.blocks) as image:
                print(json.dumps(image.verify(), sort_keys=True))
        elif args.command == "verify":
            with Image.open(args.image) as image:
                print(json.dumps(image.verify(), sort_keys=True))
        elif args.command == "ls":
            with Image.open(args.image) as image:
                for entry in image.listdir(args.path):
                    child_path = args.path.rstrip("/") + "/" + entry.name
                    size = image.lookup(child_path).size
                    print(f"{'d' if entry.kind == 2 else 'f'}\t{size}\t{entry.name}")
        elif args.command == "mkdir":
            with Image.open(args.image, writable=True) as image:
                image.mkdir(args.path)
                print(json.dumps(image.verify(), sort_keys=True))
        elif args.command == "put":
            with Image.open(args.image, writable=True) as image:
                image.put_file(args.source, args.path)
                print(json.dumps(image.verify(), sort_keys=True))
        elif args.command == "get":
            with Image.open(args.image) as image:
                image.extract_file(args.path, args.destination)
                print(f"extracted {args.path} -> {args.destination}")
        elif args.command == "rm":
            with Image.open(args.image, writable=True) as image:
                image.remove(args.path)
                print(json.dumps(image.verify(), sort_keys=True))
    except (OSError, ValueError, CorruptImage, NoSpace) as exc:
        print(f"ShizukuFS: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
