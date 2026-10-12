"""Applies IPS and BPS ROM patches (translations, MSU-1 hacks, fixes)."""

from __future__ import annotations

import zlib
from pathlib import Path


class PatchError(Exception):
    pass


def apply(rom: bytes, patch_file: Path) -> bytes:
    patch = patch_file.read_bytes()
    if patch.startswith(b"PATCH"):
        return _apply_ips(rom, patch)
    if patch.startswith(b"BPS1"):
        return _apply_bps(rom, patch)
    raise PatchError(f"'{patch_file.name}' não é um patch IPS nem BPS.")


def _apply_ips(rom: bytes, patch: bytes) -> bytes:
    out = bytearray(rom)
    pos = 5
    while True:
        if pos + 3 > len(patch):
            raise PatchError("patch IPS truncado")
        offset = int.from_bytes(patch[pos:pos + 3], "big")
        pos += 3
        if offset == 0x454F46 and pos >= len(patch) - 3:  # "EOF", optionally followed by a truncation size
            if len(patch) - pos == 3:
                del out[int.from_bytes(patch[pos:pos + 3], "big"):]
            return bytes(out)
        size = int.from_bytes(patch[pos:pos + 2], "big")
        pos += 2
        if size == 0:  # RLE record
            run = int.from_bytes(patch[pos:pos + 2], "big")
            data = patch[pos + 2:pos + 3] * run
            pos += 3
        else:
            data = patch[pos:pos + size]
            pos += size
        if len(out) < offset + len(data):
            out.extend(b"\x00" * (offset + len(data) - len(out)))
        out[offset:offset + len(data)] = data


def _apply_bps(rom: bytes, patch: bytes) -> bytes:
    if zlib.crc32(patch[:-4]) != int.from_bytes(patch[-4:], "little"):
        raise PatchError("patch BPS corrompido")
    if zlib.crc32(rom) != int.from_bytes(patch[-12:-8], "little"):
        raise PatchError("este patch BPS foi feito para outra versão da ROM")

    pos = 4

    def number() -> int:
        nonlocal pos
        value, shift = 0, 1
        while True:
            byte = patch[pos]
            pos += 1
            value += (byte & 0x7F) * shift
            if byte & 0x80:
                return value
            shift <<= 7
            value += shift

    number()  # source size
    target_size = number()
    metadata_size = number()
    pos += metadata_size

    out = bytearray(target_size)
    out_pos = source_rel = target_rel = 0
    end = len(patch) - 12
    while pos < end:
        command = number()
        action, length = command & 3, (command >> 2) + 1
        if action == 0:  # SourceRead
            out[out_pos:out_pos + length] = rom[out_pos:out_pos + length]
            out_pos += length
        elif action == 1:  # TargetRead
            out[out_pos:out_pos + length] = patch[pos:pos + length]
            pos += length
            out_pos += length
        else:
            offset = number()
            offset = -(offset >> 1) if offset & 1 else offset >> 1
            if action == 2:  # SourceCopy
                source_rel += offset
                out[out_pos:out_pos + length] = rom[source_rel:source_rel + length]
                source_rel += length
                out_pos += length
            else:  # TargetCopy: may overlap the bytes being written, so copy one at a time
                target_rel += offset
                for _ in range(length):
                    out[out_pos] = out[target_rel]
                    out_pos += 1
                    target_rel += 1

    result = bytes(out)
    if zlib.crc32(result) != int.from_bytes(patch[-8:-4], "little"):
        raise PatchError("o resultado do patch BPS não confere")
    return result
