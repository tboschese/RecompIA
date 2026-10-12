"""Static code discovery (recursive descent) with M/X flag tracking.

On the 65816 the length of immediate operands depends on the M and X flags, so the same bytes
decode differently in 8-bit and 16-bit mode. The analysis follows every reachable path from the
entry points, carrying the flag state, and records what it can't resolve statically (indirect
jumps, code in RAM, unknown flags) so later passes (traces, manual annotations) can fill it in.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from typing import Deque, Dict, List, Optional, Set, Tuple

from . import cpu
from .rom import Rom


@dataclass(frozen=True)
class State:
    m8: Optional[bool]
    x8: Optional[bool]
    emulation: Optional[bool] = False


@dataclass
class Instruction:
    pc: int
    offset: int
    opcode: cpu.Opcode
    operand: bytes
    state: State

    @property
    def size(self) -> int:
        return 1 + len(self.operand)

    @property
    def text(self) -> str:
        operand = cpu.format_operand(self.opcode, self.operand, self.pc)
        return f"{self.opcode.mnemonic} {operand}".strip()


@dataclass
class Issue:
    pc: int
    kind: str
    detail: str


@dataclass
class Analysis:
    rom: Rom
    instructions: Dict[int, Instruction] = field(default_factory=dict)  # by file offset
    labels: Dict[int, str] = field(default_factory=dict)  # by CPU address
    functions: Set[int] = field(default_factory=set)
    issues: List[Issue] = field(default_factory=list)

    @property
    def code_bytes(self) -> int:
        covered = set()
        for ins in self.instructions.values():
            covered.update(range(ins.offset, ins.offset + ins.size))
        return len(covered)


def analyze(rom: Rom, entries: List[Tuple[int, State, str]]) -> Analysis:
    result = Analysis(rom)
    queue: Deque[Tuple[int, State]] = deque()
    seen: Set[Tuple[int, State]] = set()

    def enqueue(pc: int, state: State, label: Optional[str] = None, function: bool = False) -> None:
        if label and pc not in result.labels:
            result.labels[pc] = label
        if function:
            result.functions.add(pc)
        if (pc, state) not in seen:
            seen.add((pc, state))
            queue.append((pc, state))

    for pc, state, name in entries:
        enqueue(pc, state, name, function=True)

    while queue:
        pc, state = queue.popleft()
        _trace(result, pc, state, enqueue)
    return result


def _trace(result: Analysis, pc: int, state: State, enqueue) -> None:
    rom = result.rom
    carry: Optional[bool] = None
    while True:
        offset = rom.offset(pc)
        if offset is None:
            result.issues.append(Issue(pc, "ram-code", "execução em RAM ou fora da ROM"))
            return
        opcode = cpu.OPCODES[rom.data[offset]]
        size = cpu.length(opcode, state.m8, state.x8)
        if size is None:
            flag = "M" if opcode.mode == cpu.IMM_M else "X"
            result.issues.append(Issue(pc, "unknown-flag", f"{opcode.mnemonic} imediato com flag {flag} desconhecida"))
            return
        operand = rom.data[offset + 1:offset + size]

        existing = result.instructions.get(offset)
        if existing is not None and existing.size != size:
            result.issues.append(Issue(pc, "conflict", f"decodificado como {existing.size} e {size} bytes"))
        if existing is None:
            result.instructions[offset] = Instruction(pc, offset, opcode, operand, state)
        elif existing.state == state:
            return  # this path was already followed with the same flags

        mnemonic = opcode.mnemonic
        value = int.from_bytes(operand, "little")
        bank = pc & 0xFF0000
        next_pc = bank | ((pc + size) & 0xFFFF)

        if mnemonic == "REP" and not state.emulation:
            state = State(False if value & 0x20 else state.m8, False if value & 0x10 else state.x8, state.emulation)
        elif mnemonic == "SEP":
            state = State(True if value & 0x20 else state.m8, True if value & 0x10 else state.x8, state.emulation)
        elif mnemonic == "PLP":
            state = State(True, True, True) if state.emulation else State(None, None, state.emulation)
        elif mnemonic in ("CLC", "SEC"):
            carry = mnemonic == "SEC"
        elif mnemonic == "XCE":
            emulation = carry
            state = State(True, True, True) if emulation else State(state.m8, state.x8, emulation)
            carry = None

        if mnemonic in cpu.BRANCHES or mnemonic in ("BRA", "BRL"):
            target = int(cpu.format_operand(opcode, operand, pc)[1:], 16)
            enqueue(target, state, f"loc_{target:06X}")
        elif mnemonic == "JSR" and opcode.mode == cpu.ABS:
            enqueue(bank | value, state, f"sub_{bank | value:06X}", function=True)
        elif mnemonic == "JSL":
            enqueue(value, state, f"sub_{value:06X}", function=True)
        elif mnemonic == "JMP" and opcode.mode == cpu.ABS:
            enqueue(bank | value, state, f"loc_{bank | value:06X}")
        elif mnemonic == "JML" and opcode.mode == cpu.ABSL:
            enqueue(value, state, f"loc_{value:06X}")
        elif opcode.mode in (cpu.ABSIND, cpu.ABSINDX, cpu.ABSINDL):
            result.issues.append(Issue(pc, "indirect", f"{mnemonic} {cpu.format_operand(opcode, operand, pc)}"))

        if mnemonic in cpu.TERMINATORS:
            return
        pc = next_pc
