"""ShizukuFS v0 little-endian host implementation.

The on-disk specification is in README.md. This module intentionally uses
only the Python standard library and reads/writes the documented structures;
it is not a FAT derivative, DOS driver, or Windows 98 IFS implementation.
"""

from __future__ import annotations

import io
import os
import struct
import unicodedata
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterator


BLOCK_SIZE = 4096
DATA_HEADER_SIZE = 16
DATA_PAYLOAD_SIZE = BLOCK_SIZE - DATA_HEADER_SIZE
INODE_SIZE = 128
INODE_COUNT = 256
INODE_BLOCKS = INODE_COUNT * INODE_SIZE // BLOCK_SIZE
SUPER_HEADER_SIZE = 128
MIN_BLOCKS = 32
MAX_TOOL_BLOCKS = 1 << 22  # 16 GiB logical image; at most 512 KiB per bitmap.
MAX_CHAIN_BLOCKS = 1 << 16  # Bound one operation's block list and verification.
MAX_INLINE_READ_BYTES = 64 << 20
SPARSE_REQUIRED_BYTES = 4 << 30
MAX_INITIAL_ALLOCATED_BYTES = 64 << 20
ROOT_INODE = 1
KIND_FILE = 1
KIND_DIRECTORY = 2
MAGIC_SUPER = b"SHIZFS00"
MAGIC_INODE = b"SZIN"
MAGIC_DATA = b"SZDB"
MAGIC_DIRECTORY = b"SZDR"

# The superblock is 80 bytes followed by zeroes to 128, then zeroes to 4 KiB.
# The last field is its CRC32, calculated with that field set to zero.
SUPER_STRUCT = struct.Struct("<8sHHIIQIIIIIQQIIII")
SUPER_CRC_OFFSET = SUPER_STRUCT.size - 4
INODE_STRUCT = struct.Struct("<4sIBBHIQIIIQ")
DATA_STRUCT = struct.Struct("<4sIII")
DIRECTORY_STRUCT = struct.Struct("<4sI")
ENTRY_STRUCT = struct.Struct("<IBBH")


class CorruptImage(ValueError):
    """An image violates the v0 layout or one of its checksums."""


class UnsupportedVersion(CorruptImage):
    """A valid ShizukuFS superblock uses an unknown format version."""


class NoSpace(OSError):
    """The copy-on-write transaction cannot reserve required free blocks."""


class BlockDeviceError(OSError):
    """The host file could not supply an exact block or byte range."""


class ResourceLimit(OSError):
    """A host safety bound or sparse-storage requirement was exceeded."""


def _crc(data: bytes | bytearray) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _get_bit(bitmap: bytes | bytearray, block: int) -> bool:
    return bool(bitmap[block >> 3] & (1 << (block & 7)))


def _set_bit(bitmap: bytearray, block: int, allocated: bool) -> None:
    mask = 1 << (block & 7)
    if allocated:
        bitmap[block >> 3] |= mask
    else:
        bitmap[block >> 3] &= ~mask & 0xFF


def _mark_allocated_range(bitmap: bytearray, start: int, end: int) -> None:
    """Set a contiguous bitmap range without looping over millions of bits."""
    head_end = min(end, (start + 7) & ~7)
    for block in range(start, head_end):
        _set_bit(bitmap, block, True)
    whole_start = head_end // 8
    whole_end = end // 8
    if whole_end > whole_start:
        bitmap[whole_start:whole_end] = b"\xff" * (whole_end - whole_start)
    for block in range(max(head_end, whole_end * 8), end):
        _set_bit(bitmap, block, True)


def _enable_sparse(handle: BinaryIO) -> None:
    """Require Windows sparse-file support before extending a large image."""
    if os.name != "nt":
        return
    import ctypes
    import msvcrt
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    device_io_control = kernel32.DeviceIoControl
    device_io_control.argtypes = (
        wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
        ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
        ctypes.c_void_p,
    )
    device_io_control.restype = wintypes.BOOL
    returned = wintypes.DWORD()
    # FSCTL_SET_SPARSE with a NULL input buffer sets the sparse attribute.
    if not device_io_control(msvcrt.get_osfhandle(handle.fileno()), 0x900C4,
                             None, 0, None, 0, ctypes.byref(returned), None):
        error = ctypes.get_last_error()
        raise ResourceLimit(f"filesystem cannot mark image sparse (Win32 {error})")


def _extend_windows_sparse(handle: BinaryIO, logical_bytes: int) -> None:
    """Extend with a 64-bit Win32 offset; CRT truncate can allocate holes."""
    import ctypes
    import msvcrt
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    set_pointer = kernel32.SetFilePointerEx
    set_pointer.argtypes = (wintypes.HANDLE, ctypes.c_longlong,
                            ctypes.c_void_p, wintypes.DWORD)
    set_pointer.restype = wintypes.BOOL
    set_end = kernel32.SetEndOfFile
    set_end.argtypes = (wintypes.HANDLE,)
    set_end.restype = wintypes.BOOL
    native = msvcrt.get_osfhandle(handle.fileno())
    if not set_pointer(native, logical_bytes, None, 0):
        raise OSError(ctypes.get_last_error(), "SetFilePointerEx failed")
    if not set_end(native):
        raise OSError(ctypes.get_last_error(), "SetEndOfFile failed")
    handle.seek(0)


def _allocated_bytes(path: Path) -> int | None:
    """Report physical allocation, not the potentially huge logical length."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        get_size = kernel32.GetCompressedFileSizeW
        get_size.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD))
        get_size.restype = wintypes.DWORD
        high = wintypes.DWORD()
        ctypes.set_last_error(0)
        low = get_size(str(path.resolve()), ctypes.byref(high))
        error = ctypes.get_last_error()
        if low == 0xFFFFFFFF and error:
            raise OSError(error, f"cannot measure allocated bytes for {path}")
        return (high.value << 32) | low
    stat = path.stat()
    return stat.st_blocks * 512 if hasattr(stat, "st_blocks") else None


def _prepare_sparse_file(path: Path, handle: BinaryIO, logical_bytes: int) -> None:
    if logical_bytes < SPARSE_REQUIRED_BYTES:
        return
    _enable_sparse(handle)
    if os.name == "nt":
        _extend_windows_sparse(handle, logical_bytes)
    else:
        # A small trial detects filesystems that eagerly allocate on truncate
        # before we ask for a multi-gigabyte logical length.
        handle.truncate(64 << 20)
        trial = _allocated_bytes(path)
        if trial is None or trial > 4 << 20:
            raise ResourceLimit("filesystem did not create a sparse trial file")
        handle.truncate(logical_bytes)
    allocated = _allocated_bytes(path)
    if allocated is None or allocated > MAX_INITIAL_ALLOCATED_BYTES:
        raise ResourceLimit("large image consumed too much physical storage")


@dataclass(frozen=True)
class Superblock:
    total_blocks: int
    bitmap_blocks: int
    data_start: int
    slot: int
    generation: int
    free_blocks: int
    free_inodes: int
    metadata_crc: int

    @property
    def metadata_blocks(self) -> int:
        return self.bitmap_blocks + INODE_BLOCKS

    def metadata_start(self, slot: int) -> int:
        return 2 + slot * self.metadata_blocks

    def to_block(self) -> bytes:
        header = bytearray(SUPER_HEADER_SIZE)
        SUPER_STRUCT.pack_into(
            header, 0,
            MAGIC_SUPER, 0, 0, SUPER_HEADER_SIZE, BLOCK_SIZE,
            self.total_blocks, self.bitmap_blocks, INODE_BLOCKS,
            INODE_COUNT, self.data_start, self.slot,
            self.generation, self.free_blocks, self.free_inodes,
            self.metadata_crc, ROOT_INODE, 0,
        )
        struct.pack_into("<I", header, SUPER_CRC_OFFSET, _crc(header))
        return bytes(header) + bytes(BLOCK_SIZE - SUPER_HEADER_SIZE)

    @classmethod
    def from_block(cls, block: bytes, slot: int, actual_size: int) -> Superblock:
        if len(block) != BLOCK_SIZE or block[:8] != MAGIC_SUPER:
            raise CorruptImage(f"superblock {slot}: missing magic")
        fields = SUPER_STRUCT.unpack_from(block)
        (
            _magic, major, minor, header_size, block_size, total_blocks,
            bitmap_blocks, inode_blocks, inode_count, data_start,
            active_slot, generation, free_blocks, free_inodes,
            metadata_crc, root_inode, stored_crc,
        ) = fields
        header = bytearray(block[:SUPER_HEADER_SIZE])
        struct.pack_into("<I", header, SUPER_CRC_OFFSET, 0)
        if stored_crc != _crc(header):
            raise CorruptImage(f"superblock {slot}: CRC mismatch")
        if (major, minor) != (0, 0):
            raise UnsupportedVersion(f"ShizukuFS version {major}.{minor}")
        if header_size != SUPER_HEADER_SIZE or block_size != BLOCK_SIZE:
            raise CorruptImage(f"superblock {slot}: invalid structure size")
        if any(block[SUPER_STRUCT.size:]):
            raise CorruptImage(f"superblock {slot}: reserved bytes are nonzero")
        if total_blocks < MIN_BLOCKS or total_blocks > MAX_TOOL_BLOCKS:
            raise CorruptImage(f"superblock {slot}: invalid block count")
        expected_bitmap_blocks = (total_blocks + BLOCK_SIZE * 8 - 1) // (BLOCK_SIZE * 8)
        if bitmap_blocks != expected_bitmap_blocks or inode_blocks != INODE_BLOCKS:
            raise CorruptImage(f"superblock {slot}: invalid metadata dimensions")
        if inode_count != INODE_COUNT or root_inode != ROOT_INODE:
            raise CorruptImage(f"superblock {slot}: invalid inode dimensions")
        if data_start != 2 + 2 * (bitmap_blocks + INODE_BLOCKS):
            raise CorruptImage(f"superblock {slot}: data start does not match layout")
        if data_start >= total_blocks or active_slot != slot or not generation:
            raise CorruptImage(f"superblock {slot}: invalid active generation")
        if actual_size != total_blocks * BLOCK_SIZE:
            raise CorruptImage("image length does not match superblock")
        if free_blocks > total_blocks - data_start or free_inodes > INODE_COUNT - 2:
            raise CorruptImage(f"superblock {slot}: impossible free count")
        return cls(
            total_blocks, bitmap_blocks, data_start, slot,
            generation, free_blocks, free_inodes, metadata_crc,
        )


@dataclass(frozen=True)
class Inode:
    number: int
    kind: int
    parent: int
    size: int
    first_block: int
    block_count: int
    content_crc: int
    generation: int

    def to_bytes(self) -> bytes:
        raw = bytearray(INODE_SIZE)
        INODE_STRUCT.pack_into(
            raw, 0, MAGIC_INODE, self.number, self.kind, 0, 0,
            self.parent, self.size, self.first_block, self.block_count,
            self.content_crc, self.generation,
        )
        return bytes(raw)

    @classmethod
    def from_bytes(cls, raw: bytes, number: int) -> Inode | None:
        if len(raw) != INODE_SIZE:
            raise CorruptImage("short inode entry")
        if not any(raw):
            return None
        (
            magic, stored_number, kind, flags, reserved, parent, size,
            first_block, block_count, content_crc, generation,
        ) = INODE_STRUCT.unpack_from(raw)
        if magic != MAGIC_INODE or stored_number != number:
            raise CorruptImage(f"inode {number}: bad magic or number")
        if flags or reserved or any(raw[INODE_STRUCT.size:]):
            raise CorruptImage(f"inode {number}: reserved field is nonzero")
        if kind not in (KIND_FILE, KIND_DIRECTORY):
            raise CorruptImage(f"inode {number}: invalid kind")
        if not generation:
            raise CorruptImage(f"inode {number}: zero generation")
        return cls(number, kind, parent, size, first_block,
                   block_count, content_crc, generation)


@dataclass(frozen=True)
class DirectoryEntry:
    name: str
    inode: int
    kind: int


def _encode_name(name: str) -> bytes:
    if (not name or name in (".", "..") or "/" in name or "\x00" in name or
            unicodedata.normalize("NFC", name) != name):
        raise ValueError(f"invalid non-canonical filename: {name!r}")
    raw = name.encode("utf-8", errors="strict")
    if len(raw) > 255:
        raise ValueError("filename exceeds 255 UTF-8 bytes")
    return raw


def _encode_directory(entries: list[DirectoryEntry]) -> bytes:
    ordered = sorted(entries, key=lambda item: _encode_name(item.name))
    if len(ordered) > INODE_COUNT - 2:
        raise NoSpace("directory has more entries than v0 inode capacity")
    out = bytearray(DIRECTORY_STRUCT.pack(MAGIC_DIRECTORY, len(ordered)))
    previous: bytes | None = None
    for entry in ordered:
        name = _encode_name(entry.name)
        if name == previous:
            raise ValueError(f"duplicate directory entry: {entry.name}")
        if entry.kind not in (KIND_FILE, KIND_DIRECTORY):
            raise ValueError("directory entry has invalid kind")
        out += ENTRY_STRUCT.pack(entry.inode, entry.kind, len(name), 0)
        out += name
        out += bytes((-len(name)) & 3)
        previous = name
    return bytes(out)


def _decode_directory(data: bytes) -> list[DirectoryEntry]:
    if len(data) < DIRECTORY_STRUCT.size:
        raise CorruptImage("directory has no header")
    magic, count = DIRECTORY_STRUCT.unpack_from(data)
    if magic != MAGIC_DIRECTORY or count > INODE_COUNT - 2:
        raise CorruptImage("directory header is invalid")
    entries: list[DirectoryEntry] = []
    offset = DIRECTORY_STRUCT.size
    previous: bytes | None = None
    for _ in range(count):
        if offset + ENTRY_STRUCT.size > len(data):
            raise CorruptImage("directory entry header is truncated")
        inode, kind, name_length, reserved = ENTRY_STRUCT.unpack_from(data, offset)
        offset += ENTRY_STRUCT.size
        if not name_length or reserved or kind not in (KIND_FILE, KIND_DIRECTORY):
            raise CorruptImage("directory entry header is invalid")
        if offset + name_length > len(data):
            raise CorruptImage("directory entry name is truncated")
        raw_name = data[offset:offset + name_length]
        offset += name_length
        pad = (-name_length) & 3
        if offset + pad > len(data) or any(data[offset:offset + pad]):
            raise CorruptImage("directory entry padding is invalid")
        offset += pad
        try:
            name = raw_name.decode("utf-8", errors="strict")
            _encode_name(name)
        except (UnicodeError, ValueError) as exc:
            raise CorruptImage("directory contains invalid UTF-8 name") from exc
        if previous is not None and raw_name <= previous:
            raise CorruptImage("directory entries are unsorted or duplicated")
        entries.append(DirectoryEntry(name, inode, kind))
        previous = raw_name
    if offset != len(data):
        raise CorruptImage("directory contains trailing bytes")
    return entries


def _path_parts(path: str) -> list[str]:
    if path == "/":
        return []
    if not path.startswith("/") or path.endswith("/") or "//" in path:
        raise ValueError("ShizukuFS paths must be canonical absolute / paths")
    parts = path[1:].split("/")
    for part in parts:
        _encode_name(part)
    return parts


class Image:
    """Opened ShizukuFS image with verified superblock and metadata CRC."""

    def __init__(self, path: Path, handle: BinaryIO, superblock: Superblock,
                 bitmap: bytearray, inodes: bytearray, writable: bool):
        self.path = path
        self.handle = handle
        self.super = superblock
        self.bitmap = bitmap
        self.inodes = inodes
        self.writable = writable

    @classmethod
    def create(cls, path: str | Path, blocks: int) -> Image:
        path = Path(path)
        if not MIN_BLOCKS <= blocks <= MAX_TOOL_BLOCKS:
            raise ValueError(f"v0 host creator requires {MIN_BLOCKS}..{MAX_TOOL_BLOCKS} blocks")
        bitmap_blocks = (blocks + BLOCK_SIZE * 8 - 1) // (BLOCK_SIZE * 8)
        data_start = 2 + 2 * (bitmap_blocks + INODE_BLOCKS)
        if data_start + 1 >= blocks:
            raise ValueError("image leaves no usable data blocks")
        bitmap = bytearray(bitmap_blocks * BLOCK_SIZE)
        _mark_allocated_range(bitmap, 0, data_start)
        _mark_allocated_range(bitmap, blocks, len(bitmap) * 8)
        _set_bit(bitmap, data_start, True)
        inodes = bytearray(INODE_BLOCKS * BLOCK_SIZE)
        root_data = _encode_directory([])
        root = Inode(ROOT_INODE, KIND_DIRECTORY, ROOT_INODE,
                     len(root_data), data_start, 1, _crc(root_data), 1)
        inodes[INODE_SIZE:2 * INODE_SIZE] = root.to_bytes()
        metadata = bytes(bitmap) + bytes(inodes)
        superblock = Superblock(blocks, bitmap_blocks, data_start, 0, 1,
                                blocks - data_start - 1, INODE_COUNT - 2,
                                _crc(metadata))
        created = False
        try:
            with path.open("x+b") as handle:
                created = True
                logical_bytes = blocks * BLOCK_SIZE
                if logical_bytes >= SPARSE_REQUIRED_BYTES:
                    _prepare_sparse_file(path, handle, logical_bytes)
                else:
                    handle.truncate(logical_bytes)
                cls._write_data_block(handle, data_start, ROOT_INODE, 0,
                                      root_data)
                handle.seek(superblock.metadata_start(0) * BLOCK_SIZE)
                handle.write(metadata)
                handle.seek(0)
                handle.write(superblock.to_block())
                handle.flush()
                os.fsync(handle.fileno())
                if logical_bytes >= SPARSE_REQUIRED_BYTES:
                    allocated = _allocated_bytes(path)
                    if allocated is None or allocated > MAX_INITIAL_ALLOCATED_BYTES:
                        raise ResourceLimit("large image is not physically sparse")
        except BaseException:
            # Failed creation is not a valid image and should not be mistaken
            # for an existing filesystem on the next attempt.
            if created and path.exists():
                path.unlink()
            raise
        return cls.open(path, writable=True)

    @classmethod
    def open(cls, path: str | Path, writable: bool = False) -> Image:
        path = Path(path)
        handle = path.open("r+b" if writable else "rb")
        try:
            actual_size = path.stat().st_size
            if actual_size < MIN_BLOCKS * BLOCK_SIZE or actual_size % BLOCK_SIZE:
                raise CorruptImage("image length is not a whole v0 block count")
            candidates: list[tuple[Superblock, bytearray, bytearray]] = []
            versions: list[UnsupportedVersion] = []
            for slot in (0, 1):
                handle.seek(slot * BLOCK_SIZE)
                block = handle.read(BLOCK_SIZE)
                try:
                    superblock = Superblock.from_block(block, slot, actual_size)
                    handle.seek(superblock.metadata_start(slot) * BLOCK_SIZE)
                    metadata = handle.read(superblock.metadata_blocks * BLOCK_SIZE)
                    if (len(metadata) != superblock.metadata_blocks * BLOCK_SIZE or
                            _crc(metadata) != superblock.metadata_crc):
                        raise CorruptImage(f"metadata slot {slot}: CRC mismatch")
                except UnsupportedVersion as exc:
                    versions.append(exc)
                    continue
                except CorruptImage:
                    continue
                bitmap_size = superblock.bitmap_blocks * BLOCK_SIZE
                candidates.append((superblock, bytearray(metadata[:bitmap_size]),
                                   bytearray(metadata[bitmap_size:])))
            if not candidates:
                if versions:
                    raise versions[0]
                raise CorruptImage("no valid ShizukuFS superblock/metadata generation")
            superblock, bitmap, inodes = max(candidates,
                                             key=lambda item: item[0].generation)
            return cls(path, handle, superblock, bitmap, inodes, writable)
        except BaseException:
            handle.close()
            raise

    def close(self) -> None:
        self.handle.close()

    def __enter__(self) -> Image:
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self.close()

    def _read_block(self, number: int) -> bytes:
        self.handle.seek(number * BLOCK_SIZE)
        data = self.handle.read(BLOCK_SIZE)
        if len(data) != BLOCK_SIZE:
            raise BlockDeviceError(f"short read at block {number}")
        return data

    @staticmethod
    def _write_data_block(handle: BinaryIO, number: int, owner: int,
                          next_block: int, data: bytes) -> None:
        if len(data) > DATA_PAYLOAD_SIZE:
            raise ValueError("data payload exceeds v0 block capacity")
        payload = data + bytes(DATA_PAYLOAD_SIZE - len(data))
        header = DATA_STRUCT.pack(MAGIC_DATA, owner, next_block, _crc(payload))
        handle.seek(number * BLOCK_SIZE)
        handle.write(header + payload)

    def _inode(self, number: int) -> Inode | None:
        if not 0 <= number < INODE_COUNT:
            raise CorruptImage(f"inode {number} is out of bounds")
        start = number * INODE_SIZE
        return Inode.from_bytes(bytes(self.inodes[start:start + INODE_SIZE]), number)

    def _content_chunks(self, inode: Inode,
                        seen_global: set[int] | None = None) -> Iterator[bytes]:
        if inode.block_count > MAX_CHAIN_BLOCKS:
            raise ResourceLimit(f"inode {inode.number}: chain exceeds host scan limit")
        if inode.block_count != (inode.size + DATA_PAYLOAD_SIZE - 1) // DATA_PAYLOAD_SIZE:
            raise CorruptImage(f"inode {inode.number}: size/chain length mismatch")
        if not inode.block_count:
            if inode.first_block or inode.content_crc:
                raise CorruptImage(f"inode {inode.number}: invalid empty content")
            return
        current = inode.first_block
        visited: set[int] = set()
        remaining = inode.size
        content_crc = 0
        for _ in range(inode.block_count):
            if (current < self.super.data_start or current >= self.super.total_blocks or
                    current in visited or not _get_bit(self.bitmap, current)):
                raise CorruptImage(f"inode {inode.number}: bad data chain block {current}")
            visited.add(current)
            if seen_global is not None:
                if current in seen_global:
                    raise CorruptImage(f"data block {current} has multiple owners")
                seen_global.add(current)
            block = self._read_block(current)
            magic, owner, next_block, block_crc = DATA_STRUCT.unpack_from(block)
            payload = block[DATA_HEADER_SIZE:]
            if magic != MAGIC_DATA or owner != inode.number or block_crc != _crc(payload):
                raise CorruptImage(f"inode {inode.number}: corrupt data block {current}")
            take = min(remaining, DATA_PAYLOAD_SIZE)
            if any(payload[take:]):
                raise CorruptImage(f"inode {inode.number}: nonzero data padding {current}")
            chunk = payload[:take]
            content_crc = zlib.crc32(chunk, content_crc)
            remaining -= take
            current = next_block
            yield chunk
        if current or remaining or (content_crc & 0xFFFFFFFF) != inode.content_crc:
            raise CorruptImage(f"inode {inode.number}: final chain/content CRC mismatch")

    def _read_content(self, inode: Inode) -> bytes:
        return b"".join(self._content_chunks(inode))

    def _directory_entries(self, inode: Inode) -> list[DirectoryEntry]:
        if inode.kind != KIND_DIRECTORY:
            raise NotADirectoryError(f"inode {inode.number} is not a directory")
        return _decode_directory(self._read_content(inode))

    def lookup(self, path: str) -> Inode:
        current = self._inode(ROOT_INODE)
        if current is None:
            raise CorruptImage("root inode is free")
        for name in _path_parts(path):
            entries = self._directory_entries(current)
            match = next((entry for entry in entries if entry.name == name), None)
            if match is None:
                raise FileNotFoundError(path)
            child = self._inode(match.inode)
            if child is None or child.kind != match.kind:
                raise CorruptImage(f"directory entry {name!r} refers to invalid inode")
            current = child
        return current

    def _parent_and_name(self, path: str) -> tuple[Inode, str, list[DirectoryEntry]]:
        parts = _path_parts(path)
        if not parts:
            raise ValueError("the root directory cannot be replaced")
        parent_path = "/" + "/".join(parts[:-1]) if len(parts) > 1 else "/"
        parent = self.lookup(parent_path)
        return parent, parts[-1], self._directory_entries(parent)

    def listdir(self, path: str = "/") -> list[DirectoryEntry]:
        return self._directory_entries(self.lookup(path))

    def read_file(self, path: str) -> bytes:
        inode = self.lookup(path)
        if inode.kind != KIND_FILE:
            raise IsADirectoryError(path)
        if inode.size > MAX_INLINE_READ_BYTES:
            raise ResourceLimit("read_file result exceeds host memory limit; use extract_file")
        return self._read_content(inode)

    def extract_file(self, path: str, destination: str | Path) -> None:
        inode = self.lookup(path)
        if inode.kind != KIND_FILE:
            raise IsADirectoryError(path)
        destination = Path(destination)
        # Exclusive creation avoids overwriting a user's preexisting output.
        with destination.open("xb") as output:
            try:
                for chunk in self._content_chunks(inode):
                    output.write(chunk)
            except BaseException:
                output.close()
                destination.unlink(missing_ok=True)
                raise

    def verify(self) -> dict[str, int | None]:
        if self._inode(0) is not None:
            raise CorruptImage("reserved inode zero is occupied")
        inodes: dict[int, Inode] = {}
        free_inodes = 0
        for number in range(1, INODE_COUNT):
            inode = self._inode(number)
            if inode is None:
                free_inodes += 1
            else:
                if inode.generation > self.super.generation:
                    raise CorruptImage(f"inode {number}: future generation")
                inodes[number] = inode
        root = inodes.get(ROOT_INODE)
        if not root or root.kind != KIND_DIRECTORY or root.parent != ROOT_INODE:
            raise CorruptImage("root inode is not a self-parented directory")
        if free_inodes != self.super.free_inodes:
            raise CorruptImage("free inode counter differs from inode table")

        used_blocks: set[int] = set()
        children: dict[int, list[int]] = {}
        references = {number: 0 for number in inodes}
        file_count = directory_count = 0
        for inode in inodes.values():
            if inode.parent not in inodes or inodes[inode.parent].kind != KIND_DIRECTORY:
                raise CorruptImage(f"inode {inode.number}: invalid parent")
            if inode.kind == KIND_DIRECTORY and inode.size < DIRECTORY_STRUCT.size:
                raise CorruptImage(f"inode {inode.number}: empty directory payload")
            chunks = self._content_chunks(inode, used_blocks)
            if inode.kind == KIND_FILE:
                file_count += 1
                for _ in chunks:
                    pass
                continue
            directory_count += 1
            entries = _decode_directory(b"".join(chunks))
            children[inode.number] = []
            for entry in entries:
                target = inodes.get(entry.inode)
                if not target or target.kind != entry.kind or target.parent != inode.number:
                    raise CorruptImage(f"directory {inode.number}: invalid child {entry.name}")
                references[entry.inode] += 1
                children[inode.number].append(entry.inode)
        if references[ROOT_INODE] or any(
            count != 1 for number, count in references.items() if number != ROOT_INODE
        ):
            raise CorruptImage("directory tree has orphan or multiply-linked inodes")
        reachable = {ROOT_INODE}
        pending = [ROOT_INODE]
        while pending:
            for child in children.get(pending.pop(), []):
                if child not in reachable:
                    reachable.add(child)
                    pending.append(child)
        if reachable != set(inodes):
            raise CorruptImage("directory tree is disconnected or cyclic")

        expected = bytearray(len(self.bitmap))
        _mark_allocated_range(expected, 0, self.super.data_start)
        _mark_allocated_range(expected, self.super.total_blocks, len(expected) * 8)
        for block in used_blocks:
            _set_bit(expected, block, True)
        if expected != self.bitmap:
            raise CorruptImage("bitmap disagrees with reserved blocks or data chains")
        free_blocks = self.super.total_blocks - self.super.data_start - len(used_blocks)
        if free_blocks != self.super.free_blocks:
            raise CorruptImage("free block counter differs from bitmap")
        return {
            "generation": self.super.generation,
            "blocks": self.super.total_blocks,
            "free_blocks": free_blocks,
            "inodes": len(inodes),
            "free_inodes": free_inodes,
            "files": file_count,
            "directories": directory_count,
            "logical_bytes": self.super.total_blocks * BLOCK_SIZE,
            "allocated_bytes": _allocated_bytes(self.path),
        }

    def _require_writable(self) -> None:
        if not self.writable:
            raise PermissionError("image was opened read-only")

    def mkdir(self, path: str) -> None:
        self._require_writable()
        self.verify()
        parent, name, entries = self._parent_and_name(path)
        if any(entry.name == name for entry in entries):
            raise FileExistsError(path)
        tx = _Transaction(self)
        number = tx.allocate_inode()
        tx.write_inode_content(number, KIND_DIRECTORY, parent.number,
                               io.BytesIO(_encode_directory([])),
                               len(_encode_directory([])), None)
        entries.append(DirectoryEntry(name, number, KIND_DIRECTORY))
        tx.rewrite_directory(parent, entries)
        tx.commit()

    def put_bytes(self, path: str, data: bytes) -> None:
        self._put_stream(path, io.BytesIO(data), len(data))

    def put_file(self, source: str | Path, path: str) -> None:
        source = Path(source)
        with source.open("rb") as stream:
            self._put_stream(path, stream, source.stat().st_size)

    def _put_stream(self, path: str, stream: BinaryIO, size: int) -> None:
        self._require_writable()
        if size < 0:
            raise ValueError("negative file length")
        if (size + DATA_PAYLOAD_SIZE - 1) // DATA_PAYLOAD_SIZE > MAX_CHAIN_BLOCKS:
            raise ResourceLimit("file exceeds host per-operation chain limit")
        self.verify()
        parent, name, entries = self._parent_and_name(path)
        existing = next((entry for entry in entries if entry.name == name), None)
        tx = _Transaction(self)
        if existing:
            inode = self._inode(existing.inode)
            if inode is None or inode.kind != KIND_FILE:
                raise IsADirectoryError(path)
            tx.write_inode_content(inode.number, KIND_FILE, parent.number,
                                   stream, size, inode)
        else:
            number = tx.allocate_inode()
            tx.write_inode_content(number, KIND_FILE, parent.number,
                                   stream, size, None)
            entries.append(DirectoryEntry(name, number, KIND_FILE))
            tx.rewrite_directory(parent, entries)
        tx.commit()

    def remove(self, path: str) -> None:
        self._require_writable()
        self.verify()
        parent, name, entries = self._parent_and_name(path)
        target = next((entry for entry in entries if entry.name == name), None)
        if not target:
            raise FileNotFoundError(path)
        inode = self._inode(target.inode)
        if inode is None:
            raise CorruptImage("directory child inode is free")
        if inode.kind == KIND_DIRECTORY and self._directory_entries(inode):
            raise OSError("directory is not empty")
        tx = _Transaction(self)
        tx.rewrite_directory(parent, [entry for entry in entries if entry.name != name])
        tx.release_inode(inode)
        tx.commit()


class _Transaction:
    """Copy-on-write data blocks and alternate metadata/superblock commit."""

    def __init__(self, image: Image):
        self.image = image
        self.bitmap = bytearray(image.bitmap)
        self.inodes = bytearray(image.inodes)
        self.free_blocks = image.super.free_blocks
        self.free_inodes = image.super.free_inodes
        self.release_blocks: set[int] = set()
        self.next_search = image.super.data_start
        self.generation = image.super.generation + 1

    def allocate_blocks(self, count: int) -> list[int]:
        if count == 0:
            return []
        if count > MAX_CHAIN_BLOCKS:
            raise ResourceLimit("allocation exceeds host per-operation chain limit")
        if count > self.free_blocks:
            raise NoSpace(f"need {count} free COW blocks, have {self.free_blocks}")
        selected: list[int] = []
        for block in range(self.next_search, self.image.super.total_blocks):
            if not _get_bit(self.bitmap, block):
                _set_bit(self.bitmap, block, True)
                selected.append(block)
                if len(selected) == count:
                    self.next_search = block + 1
                    self.free_blocks -= count
                    return selected
        raise CorruptImage("free counter exceeds available bitmap blocks")

    def allocate_inode(self) -> int:
        if not self.free_inodes:
            raise NoSpace("no free v0 inodes")
        for number in range(2, INODE_COUNT):
            start = number * INODE_SIZE
            if not any(self.inodes[start:start + INODE_SIZE]):
                self.free_inodes -= 1
                return number
        raise CorruptImage("free inode counter exceeds available inode slots")

    def _set_inode(self, inode: Inode) -> None:
        start = inode.number * INODE_SIZE
        self.inodes[start:start + INODE_SIZE] = inode.to_bytes()

    def _release_content(self, inode: Inode) -> None:
        current = inode.first_block
        for _ in range(inode.block_count):
            self.release_blocks.add(current)
            block = self.image._read_block(current)
            _, _, current, _ = DATA_STRUCT.unpack_from(block)

    def write_inode_content(self, number: int, kind: int, parent: int,
                            stream: BinaryIO, size: int,
                            old: Inode | None) -> None:
        count = (size + DATA_PAYLOAD_SIZE - 1) // DATA_PAYLOAD_SIZE
        blocks = self.allocate_blocks(count)
        remaining = size
        content_crc = 0
        for index, block in enumerate(blocks):
            take = min(remaining, DATA_PAYLOAD_SIZE)
            chunk = stream.read(take)
            if len(chunk) != take:
                raise BlockDeviceError("source changed or ended while copying")
            content_crc = zlib.crc32(chunk, content_crc)
            next_block = blocks[index + 1] if index + 1 < count else 0
            Image._write_data_block(self.image.handle, block, number,
                                    next_block, chunk)
            remaining -= take
        if remaining or stream.read(1):
            raise BlockDeviceError("source length changed while copying")
        inode = Inode(number, kind, parent, size,
                      blocks[0] if blocks else 0, count,
                      content_crc & 0xFFFFFFFF, self.generation)
        self._set_inode(inode)
        if old:
            self._release_content(old)

    def rewrite_directory(self, old: Inode,
                          entries: list[DirectoryEntry]) -> None:
        data = _encode_directory(entries)
        self.write_inode_content(old.number, KIND_DIRECTORY, old.parent,
                                 io.BytesIO(data), len(data), old)

    def release_inode(self, inode: Inode) -> None:
        self._release_content(inode)
        start = inode.number * INODE_SIZE
        self.inodes[start:start + INODE_SIZE] = bytes(INODE_SIZE)
        self.free_inodes += 1

    def commit(self) -> None:
        image = self.image
        for block in self.release_blocks:
            if not _get_bit(self.bitmap, block):
                raise CorruptImage("transaction releases an unallocated block")
            _set_bit(self.bitmap, block, False)
            self.free_blocks += 1
        metadata = bytes(self.bitmap) + bytes(self.inodes)
        slot = 1 - image.super.slot
        next_super = Superblock(
            image.super.total_blocks, image.super.bitmap_blocks,
            image.super.data_start, slot, self.generation,
            self.free_blocks, self.free_inodes, _crc(metadata),
        )
        # Flush new content before alternate metadata; flush metadata before
        # publishing the new superblock. The previous generation's referenced
        # data blocks are never overwritten by this transaction.
        image.handle.flush()
        os.fsync(image.handle.fileno())
        staged = Image(image.path, image.handle, next_super,
                       self.bitmap, self.inodes, writable=True)
        staged.verify()
        image.handle.seek(next_super.metadata_start(slot) * BLOCK_SIZE)
        image.handle.write(metadata)
        image.handle.flush()
        os.fsync(image.handle.fileno())
        image.handle.seek(slot * BLOCK_SIZE)
        image.handle.write(next_super.to_block())
        image.handle.flush()
        os.fsync(image.handle.fileno())
        image.super = next_super
        image.bitmap = self.bitmap
        image.inodes = self.inodes
