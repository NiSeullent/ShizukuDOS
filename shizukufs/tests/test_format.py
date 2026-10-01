from __future__ import annotations

import io
import os
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

from shizukufs.format import (
    BLOCK_SIZE,
    BlockDeviceError,
    DATA_HEADER_SIZE,
    DATA_PAYLOAD_SIZE,
    DirectoryEntry,
    KIND_FILE,
    MAX_CHAIN_BLOCKS,
    MAX_TOOL_BLOCKS,
    ROOT_INODE,
    ResourceLimit,
    SUPER_CRC_OFFSET,
    SUPER_HEADER_SIZE,
    _Transaction,
    CorruptImage,
    Image,
    NoSpace,
    UnsupportedVersion,
)


class ShizukuFSFormatTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.image_path = Path(self.temp.name) / "disk.szfs"

    def test_raw_layout_and_existing_image_preserved(self) -> None:
        with Image.create(self.image_path, 32) as image:
            self.assertEqual(image.verify()["free_blocks"], 11)
        raw = self.image_path.read_bytes()
        self.assertEqual(raw[:8], b"SHIZFS00")
        self.assertEqual(struct.unpack_from("<HH", raw, 8), (0, 0))
        self.assertEqual(struct.unpack_from("<I", raw, 16)[0], BLOCK_SIZE)
        self.assertEqual(struct.unpack_from("<Q", raw, 20)[0], 32)
        self.assertEqual(struct.unpack_from("<II", raw, 28), (1, 8))
        self.assertEqual(struct.unpack_from("<I", raw, 40)[0], 20)
        root_inode_offset = 2 * BLOCK_SIZE + BLOCK_SIZE + 128
        self.assertEqual(raw[root_inode_offset:root_inode_offset + 4], b"SZIN")
        self.assertEqual(raw[20 * BLOCK_SIZE:20 * BLOCK_SIZE + 4], b"SZDB")
        self.assertEqual(raw[20 * BLOCK_SIZE + DATA_HEADER_SIZE:
                             20 * BLOCK_SIZE + DATA_HEADER_SIZE + 4], b"SZDR")
        with self.assertRaises(FileExistsError):
            Image.create(self.image_path, 32)
        self.assertEqual(self.image_path.read_bytes(), raw)

    def test_nested_roundtrip_replace_and_delete(self) -> None:
        payload = bytes(range(256)) * 43 + b"\x00end"
        replacement = b"new" * 1700
        with Image.create(self.image_path, 64) as image:
            self.assertEqual(image.verify()["generation"], 1)
            image.mkdir("/문서")
            image.mkdir("/문서/nested")
            image.put_bytes("/문서/nested/긴 이름.bin", payload)
            self.assertEqual(image.read_file("/문서/nested/긴 이름.bin"), payload)
            self.assertEqual([entry.name for entry in image.listdir("/문서/nested")],
                             ["긴 이름.bin"])
            self.assertEqual(image.verify()["files"], 1)
            image.put_bytes("/문서/nested/긴 이름.bin", replacement)
            self.assertEqual(image.read_file("/문서/nested/긴 이름.bin"), replacement)
            self.assertEqual(image.verify()["files"], 1)
            out = Path(self.temp.name) / "extracted.bin"
            image.extract_file("/문서/nested/긴 이름.bin", out)
            self.assertEqual(out.read_bytes(), replacement)
            with self.assertRaises(FileExistsError):
                image.extract_file("/문서/nested/긴 이름.bin", out)
            with self.assertRaises(OSError):
                image.remove("/문서")
            image.remove("/문서/nested/긴 이름.bin")
            image.remove("/문서/nested")
            image.remove("/문서")
            self.assertEqual(image.verify()["inodes"], 1)
        with Image.open(self.image_path) as image:
            self.assertEqual(image.listdir("/"), [])
            self.assertEqual(image.verify()["files"], 0)

    def test_empty_file_and_free_block_reuse(self) -> None:
        with Image.create(self.image_path, 64) as image:
            image.put_bytes("/empty", b"")
            self.assertEqual(image.lookup("/empty").first_block, 0)
            self.assertEqual(image.read_file("/empty"), b"")
            image.remove("/empty")
            image.put_bytes("/a", b"A" * DATA_PAYLOAD_SIZE)
            old_block = image.lookup("/a").first_block
            image.remove("/a")
            after_delete = image.verify()["free_blocks"]
            image.put_bytes("/b", b"B" * DATA_PAYLOAD_SIZE)
            self.assertEqual(image.lookup("/b").first_block, old_block)
            self.assertLess(image.verify()["free_blocks"], after_delete)
            self.assertEqual(image.read_file("/b"), b"B" * DATA_PAYLOAD_SIZE)

    def test_directory_records_cross_block_boundary(self) -> None:
        with Image.create(self.image_path, 128) as image:
            names = [f"{index:02d}-" + "n" * 117 for index in range(40)]
            for name in reversed(names):
                image.put_bytes("/" + name, b"")
            self.assertGreater(image.lookup("/").block_count, 1)
            self.assertEqual([entry.name for entry in image.listdir("/")], names)
            self.assertEqual(image.verify()["files"], 40)
            for name in names[:15]:
                image.remove("/" + name)
            self.assertEqual(image.verify()["files"], 25)
            self.assertEqual([entry.name for entry in image.listdir("/")], names[15:])

    def test_out_of_space_preserves_committed_generation(self) -> None:
        payload = b"x" * (10 * DATA_PAYLOAD_SIZE)
        with Image.create(self.image_path, 32) as image:
            image.put_bytes("/keep", payload)
            generation = image.super.generation
            with self.assertRaises(NoSpace):
                image.put_bytes("/second", b"more")
            self.assertEqual(image.super.generation, generation)
            self.assertEqual(image.read_file("/keep"), payload)
            self.assertEqual(image.verify()["files"], 1)
        with Image.open(self.image_path) as image:
            self.assertEqual(image.read_file("/keep"), payload)
            with self.assertRaises(FileNotFoundError):
                image.lookup("/second")

    def test_short_source_aborts_before_metadata_commit(self) -> None:
        with Image.create(self.image_path, 64) as image:
            image.put_bytes("/file", b"previous")
            generation = image.super.generation
            with self.assertRaises(BlockDeviceError):
                image._put_stream("/file", io.BytesIO(b"short"), 4097)
            self.assertEqual(image.super.generation, generation)
            self.assertEqual(image.read_file("/file"), b"previous")
            image.verify()
        with Image.open(self.image_path) as image:
            self.assertEqual(image.read_file("/file"), b"previous")

    def test_corrupted_content_is_detected_without_silent_rollback(self) -> None:
        with Image.create(self.image_path, 64) as image:
            image.put_bytes("/file", b"sensitive content")
            block = image.lookup("/file").first_block
        with self.image_path.open("r+b") as raw:
            raw.seek(block * BLOCK_SIZE + DATA_HEADER_SIZE + 2)
            byte = raw.read(1)
            raw.seek(-1, os.SEEK_CUR)
            raw.write(bytes([byte[0] ^ 0x80]))
        with Image.open(self.image_path) as image:
            self.assertEqual(image.super.generation, 2)
            with self.assertRaisesRegex(CorruptImage, "corrupt data block"):
                image.verify()
            with self.assertRaises(CorruptImage):
                image.read_file("/file")

    def test_cycle_in_data_chain_is_detected(self) -> None:
        with Image.create(self.image_path, 64) as image:
            image.put_bytes("/file", b"A" * (DATA_PAYLOAD_SIZE + 1))
            first = image.lookup("/file").first_block
        with self.image_path.open("r+b") as raw:
            raw.seek(first * BLOCK_SIZE + 8)
            raw.write(struct.pack("<I", first))
        with Image.open(self.image_path) as image:
            with self.assertRaisesRegex(CorruptImage, "bad data chain block"):
                image.verify()

    def test_nonzero_padding_rejected_even_with_recomputed_block_crc(self) -> None:
        with Image.create(self.image_path, 64) as image:
            image.put_bytes("/file", b"abc")
            block_number = image.lookup("/file").first_block
        with self.image_path.open("r+b") as raw:
            raw.seek(block_number * BLOCK_SIZE)
            block = bytearray(raw.read(BLOCK_SIZE))
            block[DATA_HEADER_SIZE + 3] = 1
            struct.pack_into("<I", block, 12,
                             zlib.crc32(block[DATA_HEADER_SIZE:]) & 0xFFFFFFFF)
            raw.seek(block_number * BLOCK_SIZE)
            raw.write(block)
        with Image.open(self.image_path) as image:
            with self.assertRaisesRegex(CorruptImage, "nonzero data padding"):
                image.verify()

    def test_metadata_damage_recovers_prior_generation(self) -> None:
        with Image.create(self.image_path, 64) as image:
            image.mkdir("/kept")
            self.assertEqual(image.super.generation, 2)
            image.put_bytes("/kept/latest", b"new")
            latest = image.super
            self.assertEqual(latest.generation, 3)
        with self.image_path.open("r+b") as raw:
            raw.seek(latest.metadata_start(latest.slot) * BLOCK_SIZE + 10)
            byte = raw.read(1)
            raw.seek(-1, os.SEEK_CUR)
            raw.write(bytes([byte[0] ^ 1]))
        with Image.open(self.image_path) as recovered:
            self.assertEqual(recovered.super.generation, 2)
            self.assertEqual(recovered.listdir("/kept"), [])
            with self.assertRaises(FileNotFoundError):
                recovered.lookup("/kept/latest")
            recovered.verify()

    def test_superblock_damage_and_unknown_version(self) -> None:
        with Image.create(self.image_path, 64):
            pass
        with self.image_path.open("r+b") as raw:
            raw.seek(8)
            raw.write(struct.pack("<H", 1))
            raw.seek(0)
            header = bytearray(raw.read(SUPER_HEADER_SIZE))
            struct.pack_into("<I", header, SUPER_CRC_OFFSET, 0)
            struct.pack_into("<I", header, SUPER_CRC_OFFSET,
                             zlib.crc32(header) & 0xFFFFFFFF)
            raw.seek(0)
            raw.write(header)
        with self.assertRaises(UnsupportedVersion):
            Image.open(self.image_path)
        with self.image_path.open("r+b") as raw:
            raw.seek(0)
            raw.write(bytes(8))
        with self.assertRaises(CorruptImage):
            Image.open(self.image_path)

    def test_truncation_and_invalid_names(self) -> None:
        with Image.create(self.image_path, 64) as image:
            for name in ("relative", "/bad//path", "/not/", "/a/../b", "/e\u0301"):
                with self.assertRaises(ValueError):
                    image.mkdir(name)
            self.assertEqual(image.verify()["generation"], 1)
        with self.image_path.open("r+b") as raw:
            raw.truncate(self.image_path.stat().st_size - 1)
        with self.assertRaises(CorruptImage):
            Image.open(self.image_path)

    def test_8gib_sparse_image_high_offset_roundtrip_and_corruption(self) -> None:
        blocks = 2 << 20
        payload = (bytes(range(256)) * 42) + b"high-offset-end"
        source = Path(self.temp.name) / "source.bin"
        extracted = Path(self.temp.name) / "extracted.bin"
        source.write_bytes(payload)
        with Image.create(self.image_path, blocks) as image:
            report = image.verify()
            self.assertEqual(report["logical_bytes"], 8 << 30)
            self.assertLess(report["allocated_bytes"], 64 << 20)
            self.assertEqual(image.super.total_blocks, blocks)
            self.assertEqual(image.super.bitmap_blocks, 64)

            # Put one ordinary file through the public API and force a second
            # valid COW chain beyond the old 4-GiB byte-offset boundary.
            image.put_file(source, "/ordinary.bin")
            tx = _Transaction(image)
            tx.next_search = (1 << 20) + 128
            number = tx.allocate_inode()
            with source.open("rb") as stream:
                tx.write_inode_content(number, KIND_FILE, ROOT_INODE,
                                       stream, len(payload), None)
            entries = image.listdir("/")
            entries.append(DirectoryEntry("above-4g.bin", number, KIND_FILE))
            tx.rewrite_directory(image.lookup("/"), entries)
            tx.commit()
            first = image.lookup("/above-4g.bin").first_block
            self.assertGreater(first * BLOCK_SIZE, 4 << 30)
            self.assertEqual(image.read_file("/ordinary.bin"), payload)
            self.assertEqual(image.read_file("/above-4g.bin"), payload)
            self.assertEqual(image.verify()["files"], 2)
            self.assertLess(image.verify()["allocated_bytes"], 64 << 20)

        with Image.open(self.image_path) as image:
            image.extract_file("/above-4g.bin", extracted)
            self.assertEqual(extracted.read_bytes(), payload)
            self.assertEqual(image.verify()["logical_bytes"], 8 << 30)
        with self.image_path.open("r+b") as raw:
            raw.seek(first * BLOCK_SIZE + DATA_HEADER_SIZE + 3)
            original = raw.read(1)
            raw.seek(-1, os.SEEK_CUR)
            raw.write(bytes([original[0] ^ 1]))
        with Image.open(self.image_path) as image:
            with self.assertRaisesRegex(CorruptImage, "corrupt data block"):
                image.verify()

    def test_sparse_and_operation_resource_guards(self) -> None:
        with self.assertRaises(ValueError):
            Image.create(self.image_path, MAX_TOOL_BLOCKS + 1)
        with mock.patch("shizukufs.format._allocated_bytes", return_value=8 << 30):
            with self.assertRaises(ResourceLimit):
                Image.create(self.image_path, 2 << 20)
        self.assertFalse(self.image_path.exists())
        with Image.create(self.image_path, 64) as image:
            generation = image.super.generation
            with self.assertRaises(ResourceLimit):
                image._put_stream("/too-big", io.BytesIO(),
                                  (MAX_CHAIN_BLOCKS + 1) * DATA_PAYLOAD_SIZE)
            self.assertEqual(image.super.generation, generation)
            self.assertEqual(image.verify()["files"], 0)


if __name__ == "__main__":
    unittest.main()
