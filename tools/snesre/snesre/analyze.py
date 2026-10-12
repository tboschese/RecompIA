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
from .trace import OPCODE, Trace, decode, wram_index


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
class RamBlock:
    address: int                 # CPU address ($7E/$7F)
    instructions: List[Instruction]
    source: Optional[int] = None  # ROM address the bytes were copied from, if found verbatim


@dataclass
class JumpTable:
    pc: int        # the JMP/JSR (abs,X) using it
    address: int   # first entry
    targets: List[int]


@dataclass
class Analysis:
    rom: Rom
    trace: Optional[Trace] = None
    instructions: Dict[int, Instruction] = field(default_factory=dict)  # by file offset
    labels: Dict[int, str] = field(default_factory=dict)  # by CPU address
    functions: Set[int] = field(default_factory=set)
    issues: List[Issue] = field(default_factory=list)
    tables: Dict[int, JumpTable] = field(default_factory=dict)  # by file offset of the table
    data: Set[int] = field(default_factory=set)  # file offsets known to hold data (jump tables)
    ram_blocks: List["RamBlock"] = field(default_factory=list)  # code that ran from WRAM (traced)

    @property
    def code_bytes(self) -> int:
        covered = set()
        for ins in self.instructions.values():
            covered.update(range(ins.offset, ins.offset + ins.size))
        return len(covered)


def analyze(rom: Rom, entries: List[Tuple[int, State, str]], trace: Optional[Trace] = None) -> Analysis:
    result = Analysis(rom, trace)
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
    if trace is not None:
        for offset in trace.entries():
            m8, x8, emulation = trace.flags(offset)
            pc = rom.address(offset)
            enqueue(pc, State(m8, x8, emulation), f"loc_{pc:06X}")

    while queue:
        pc, state = queue.popleft()
        _trace(result, pc, state, enqueue)
    if trace is not None:
        result.ram_blocks = _ram_code(rom, trace)
    return result


RAM_BLOCK_GAP = 8
SOURCE_MIN_BYTES = 12


def _ram_code(rom: Rom, trace: Trace) -> List[RamBlock]:
    """Decodes the traced WRAM code and looks for the ROM bytes it was copied from."""
    blocks: List[RamBlock] = []
    last_end = None
    for index, value in enumerate(trace.wram):
        if not value & OPCODE:
            continue
        m8, x8, emulation = decode(value)
        opcode = cpu.OPCODES[trace.wram_code[index]]
        size = cpu.length(opcode, True if m8 is None else m8, True if x8 is None else x8)
        pc = 0x7E0000 + index
        ins = Instruction(pc, index, opcode, bytes(trace.wram_code[index + 1:index + size]), State(m8, x8, emulation))
        if last_end is None or index - last_end > RAM_BLOCK_GAP:
            blocks.append(RamBlock(pc, []))
        blocks[-1].instructions.append(ins)
        last_end = index + size

    for block in blocks:
        start = block.instructions[0].offset
        known = bytearray()
        for ins in block.instructions:
            if ins.offset != start + len(known):
                break
            known += trace.wram_code[ins.offset:ins.offset + ins.size]
        if len(known) >= SOURCE_MIN_BYTES:
            found = rom.data.find(bytes(known))
            if found >= 0:
                block.source = rom.address(found)
    return blocks


def _trace(result: Analysis, pc: int, state: State, enqueue) -> None:
    rom = result.rom
    carry: Optional[bool] = None
    recent: Deque[Instruction] = deque(maxlen=6)
    while True:
        offset = rom.offset(pc)
        if offset is None:
            index = wram_index(pc)
            if result.trace is None or index is None or not result.trace.wram[index] & OPCODE:
                result.issues.append(Issue(pc, "ram-code", "execução em RAM ou fora da ROM"))
            return
        if offset in result.data:
            result.issues.append(Issue(pc, "conflict", "caminho entra numa tabela de dados"))
            return
        if result.trace is not None:
            observed = result.trace.flags(offset)
            if observed is not None:
                m8, x8, emulation = observed
                state = State(state.m8 if m8 is None else m8, state.x8 if x8 is None else x8, emulation)
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
        instruction = Instruction(pc, offset, opcode, operand, state)
        if existing is None:
            result.instructions[offset] = instruction
        elif existing.state == state:
            return  # this path was already followed with the same flags
        recent.append(instruction)

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
        elif opcode.mode == cpu.ABSINDX and _jump_table(result, pc, bank | value, recent, state, enqueue, mnemonic == "JSR"):
            pass
        elif opcode.mode in (cpu.ABSIND, cpu.ABSINDX, cpu.ABSINDL):
            result.issues.append(Issue(pc, "indirect", f"{mnemonic} {cpu.format_operand(opcode, operand, pc)}"))

        if mnemonic in cpu.TERMINATORS:
            return
        pc = next_pc


MAX_TABLE_ENTRIES = 256
_SUSPICIOUS_OPCODES = {0x00, 0x02, 0x42, 0xDB}  # BRK, COP, WDM, STP


def _table_size(recent: Deque[Instruction]) -> Optional[int]:
    """Entry count implied by the index computation, e.g. `AND #$0F; ASL A; TAX` -> 16."""
    shifts = 0
    for ins in reversed(list(recent)[:-1]):
        name = ins.opcode.mnemonic
        if name == "ASL" and ins.opcode.mode == cpu.ACC:
            shifts += 1
        elif name == "AND" and ins.opcode.mode == cpu.IMM_M:
            mask = int.from_bytes(ins.operand, "little") << shifts
            return mask // 2 + 1 if mask and not mask & 1 and not (mask + 2) & mask else None
        elif name not in ("TAX", "TAY", "SEP", "REP", "XBA", "CLC", "PHX", "PHY"):
            return None
    return None


def _jump_table(result: Analysis, pc: int, table: int, recent, state: State, enqueue, call: bool) -> bool:
    """Reads the 16-bit pointers of a `JMP/JSR (table,X)` from the current bank and follows them."""
    rom = result.rom
    start = rom.offset(table)
    if start is None:
        return False
    bank = pc & 0xFF0000
    limit = _table_size(recent)
    exact = limit is not None
    targets: List[int] = []
    lowest_target = None
    for i in range(limit or MAX_TABLE_ENTRIES):
        entry_offset = start + 2 * i
        if entry_offset + 2 > len(rom.data):
            break
        if result.trace is not None and (result.trace.is_code(entry_offset) or result.trace.is_code(entry_offset + 1)):
            break  # the table can't overlap code that actually ran
        if not exact:
            entry_address = table + 2 * i
            if lowest_target is not None and entry_address >= lowest_target:
                break  # the table ran into the code it points to
            if i and (entry_offset in result.instructions or entry_offset + 1 in result.instructions):
                break
        target = bank | int.from_bytes(rom.data[entry_offset:entry_offset + 2], "little")
        target_offset = rom.offset(target)
        if target_offset is None or (not exact and rom.data[target_offset] in _SUSPICIOUS_OPCODES):
            break
        targets.append(target)
        if target > table and (lowest_target is None or target < lowest_target):
            lowest_target = target
    if not targets:
        return False

    result.tables[start] = JumpTable(pc, table, targets)
    result.data.update(range(start, start + 2 * len(targets)))
    if table not in result.labels:
        result.labels[table] = f"tbl_{table:06X}"
    for target in targets:
        enqueue(target, state, f"{'sub' if call else 'loc'}_{target:06X}", function=call)
    return True
