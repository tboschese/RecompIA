"""SNES cartridge loading: copier headers, internal header, memory mapping and vectors."""

from __future__ import annotations

import hashlib
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional

LOROM, HIROM = "LoROM", "HiROM"

VECTOR_NAMES = {
    0xFFE4: "native_cop", 0xFFE6: "native_brk", 0xFFE8: "native_abort", 0xFFEA: "native_nmi",
    0xFFEE: "native_irq", 0xFFF4: "emu_cop", 0xFFF8: "emu_abort", 0xFFFA: "emu_nmi",
    0xFFFC: "reset", 0xFFFE: "emu_irq_brk",
}


@dataclass
class Rom:
    data: bytes
    mapping: str
    title: str
    map_mode: int
    fastrom: bool

    @property
    def sha1(self) -> str:
        return hashlib.sha1(self.data).hexdigest()

    @property
    def crc32(self) -> str:
        return f"{zlib.crc32(self.data):08X}"

    def offset(self, address: int) -> Optional[int]:
        """File offset of a CPU address, or None if it isn't ROM (RAM, I/O, open bus)."""
        bank, addr = address >> 16, address & 0xFFFF
        if bank in (0x7E, 0x7F):
            return None
        if self.mapping == HIROM:
            if bank & 0x7F >= 0x40:
                offset = ((bank & 0x3F) << 16) | addr
            elif addr >= 0x8000:
                offset = ((bank & 0x3F) << 16) | addr
            else:
                return None
        else:
            if addr < 0x8000:
                return None
            offset = (bank & 0x7F) * 0x8000 + (addr - 0x8000)
        return offset % len(self.data) if offset < len(self.data) * 2 else None

    def read(self, address: int, size: int) -> Optional[bytes]:
        offset = self.offset(address)
        if offset is None or offset + size > len(self.data):
            return None
        return self.data[offset:offset + size]

    def vectors(self) -> Dict[str, int]:
        result = {}
        for location, name in VECTOR_NAMES.items():
            raw = self.read(location, 2)
            if raw:
                value = int.from_bytes(raw, "little")
                if value not in (0x0000, 0xFFFF):
                    result[name] = value
        return result


def load(path: Path) -> Rom:
    data = path.read_bytes()
    if len(data) % 1024 == 512:  # copier header
        data = data[512:]

    best = max((LOROM, 0x7FC0), (HIROM, 0xFFC0), key=lambda candidate: _header_score(data, candidate[1]))
    mapping, header = best
    title = data[header:header + 21].decode("ascii", "replace").strip()
    map_mode = data[header + 21]
    return Rom(data=data, mapping=mapping, title=title, map_mode=map_mode, fastrom=bool(map_mode & 0x10))


def _header_score(data: bytes, header: int) -> int:
    if header + 0x40 > len(data):
        return -1
    score = 0
    checksum = int.from_bytes(data[header + 30:header + 32], "little")
    complement = int.from_bytes(data[header + 28:header + 30], "little")
    if checksum ^ complement == 0xFFFF:
        score += 4
    title = data[header:header + 21]
    if all(0x20 <= b < 0x7F for b in title):
        score += 2
    mode = data[header + 21] & 0x0F
    if (header == 0x7FC0 and mode in (0, 2)) or (header == 0xFFC0 and mode in (1, 5)):
        score += 2
    reset = int.from_bytes(data[header + 0x3C:header + 0x3E], "little")
    if reset >= 0x8000:
        score += 1
    return score
