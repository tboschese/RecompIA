"""Execution logs: what actually ran while someone played, and with which M/X flags.

Two sources, normalized to the same per-byte flags:
- snesre's own tracer (`snesre play`), which knows exactly where each instruction starts;
- Mesen 2's code/data logger (.cdl), which marks code bytes, jump targets and subroutine entries.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Optional

OPCODE, OPERAND, M8, M16, X8, X16, JUMP_TARGET, EMULATION = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
MAGIC = b"SNESRE-TRACE2\0\0\0"
WRAM_SIZE = 0x20000

# Mesen 2 SNES CDL flags
_MESEN_CODE, _MESEN_JUMP_TARGET, _MESEN_SUB_ENTRY, _MESEN_X8, _MESEN_M8 = 0x01, 0x04, 0x08, 0x10, 0x20


@dataclass
class Trace:
    rom: bytearray
    wram: bytearray
    wram_code: bytearray  # the bytes that ran from WRAM (last seen)

    @property
    def code_bytes(self) -> int:
        return sum(1 for flags in self.rom if flags & (OPCODE | OPERAND))

    def flags(self, offset: int) -> Optional[tuple]:
        """(m8, x8, emulation) the code at this offset ran with; None for a flag seen both ways."""
        return decode(self.rom[offset])

    def is_code(self, offset: int) -> bool:
        return bool(self.rom[offset] & (OPCODE | OPERAND))

    def entries(self) -> Iterator[int]:
        """Offsets of instructions reached by a jump, call, return or interrupt."""
        for offset, value in enumerate(self.rom):
            if value & OPCODE and value & JUMP_TARGET:
                yield offset

    def merge(self, other: "Trace") -> None:
        for i, value in enumerate(other.rom):
            self.rom[i] |= value
        for i, value in enumerate(other.wram):
            if value:
                self.wram_code[i] = other.wram_code[i]
            self.wram[i] |= value


def decode(value: int) -> Optional[tuple]:
    if not value & (OPCODE | OPERAND) or not value & (M8 | M16):
        return None
    m8 = None if value & M8 and value & M16 else bool(value & M8)
    x8 = None if value & X8 and value & X16 else bool(value & X8)
    return m8, x8, bool(value & EMULATION)


def wram_index(address: int) -> Optional[int]:
    """WRAM offset of a CPU address ($7E/$7F banks and the low 8 KB mirror), or None."""
    bank, addr = address >> 16, address & 0xFFFF
    if bank in (0x7E, 0x7F):
        return (bank - 0x7E) << 16 | addr
    if (bank & 0x7F) < 0x40 and addr < 0x2000:
        return addr
    return None


def load(path: Path, rom_size: int) -> Trace:
    data = path.read_bytes()
    if data.startswith(MAGIC):
        size = int.from_bytes(data[16:20], "little")
        if size != rom_size:
            raise ValueError(f"{path.name}: rastreamento de uma ROM de outro tamanho")
        wram = 20 + size
        return Trace(bytearray(data[20:wram]), bytearray(data[wram:wram + WRAM_SIZE]),
                     bytearray(data[wram + WRAM_SIZE:wram + 2 * WRAM_SIZE]))
    return _load_mesen(path, data, rom_size)


def _load_mesen(path: Path, data: bytes, rom_size: int) -> Trace:
    header = len(data) - rom_size
    if not data.startswith(b"CDL") or not 0 <= header <= 64:
        raise ValueError(f"{path.name}: formato não reconhecido (esperado rastreamento do snesre ou .cdl do Mesen 2)")
    rom = bytearray(rom_size)
    for offset, value in enumerate(data[header:]):
        if not value & _MESEN_CODE:
            continue
        flags = OPERAND | (M8 if value & _MESEN_M8 else M16) | (X8 if value & _MESEN_X8 else X16)
        if value & (_MESEN_JUMP_TARGET | _MESEN_SUB_ENTRY):
            flags |= OPCODE | JUMP_TARGET
        rom[offset] = flags
    return Trace(rom, bytearray(WRAM_SIZE), bytearray(WRAM_SIZE))
