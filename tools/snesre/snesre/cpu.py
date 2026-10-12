"""65816 instruction set: mnemonics, addressing modes and lengths."""

from __future__ import annotations

from typing import NamedTuple, Optional

# Addressing modes. Immediate operands of "m" / "x" modes are 1 byte when the M / X flag is
# set (8-bit registers) and 2 bytes otherwise.
IMP, ACC, IMM_M, IMM_X, IMM8 = "imp", "acc", "imm_m", "imm_x", "imm8"
DP, DPX, DPY, DPIND, DPINDX, DPINDY, DPINDL, DPINDLY = "dp", "dpx", "dpy", "dpind", "dpindx", "dpindy", "dpindl", "dpindly"
ABS, ABSX, ABSY, ABSL, ABSLX = "abs", "absx", "absy", "absl", "abslx"
ABSIND, ABSINDX, ABSINDL = "absind", "absindx", "absindl"
SR, SRIY, REL8, REL16, MOVE, IMM16 = "sr", "sriy", "rel8", "rel16", "move", "imm16"

_OPERAND_BYTES = {
    IMP: 0, ACC: 0, IMM8: 1, IMM16: 2,
    DP: 1, DPX: 1, DPY: 1, DPIND: 1, DPINDX: 1, DPINDY: 1, DPINDL: 1, DPINDLY: 1,
    ABS: 2, ABSX: 2, ABSY: 2, ABSL: 3, ABSLX: 3, ABSIND: 2, ABSINDX: 2, ABSINDL: 2,
    SR: 1, SRIY: 1, REL8: 1, REL16: 2, MOVE: 2,
}

# One entry per opcode, $00-$FF.
_TABLE = """
BRK imm8|ORA dpindx|COP imm8|ORA sr|TSB dp|ORA dp|ASL dp|ORA dpindl|PHP imp|ORA imm_m|ASL acc|PHD imp|TSB abs|ORA abs|ASL abs|ORA absl
BPL rel8|ORA dpindy|ORA dpind|ORA sriy|TRB dp|ORA dpx|ASL dpx|ORA dpindly|CLC imp|ORA absy|INC acc|TCS imp|TRB abs|ORA absx|ASL absx|ORA abslx
JSR abs|AND dpindx|JSL absl|AND sr|BIT dp|AND dp|ROL dp|AND dpindl|PLP imp|AND imm_m|ROL acc|PLD imp|BIT abs|AND abs|ROL abs|AND absl
BMI rel8|AND dpindy|AND dpind|AND sriy|BIT dpx|AND dpx|ROL dpx|AND dpindly|SEC imp|AND absy|DEC acc|TSC imp|BIT absx|AND absx|ROL absx|AND abslx
RTI imp|EOR dpindx|WDM imm8|EOR sr|MVP move|EOR dp|LSR dp|EOR dpindl|PHA imp|EOR imm_m|LSR acc|PHK imp|JMP abs|EOR abs|LSR abs|EOR absl
BVC rel8|EOR dpindy|EOR dpind|EOR sriy|MVN move|EOR dpx|LSR dpx|EOR dpindly|CLI imp|EOR absy|PHY imp|TCD imp|JML absl|EOR absx|LSR absx|EOR abslx
RTS imp|ADC dpindx|PER rel16|ADC sr|STZ dp|ADC dp|ROR dp|ADC dpindl|PLA imp|ADC imm_m|ROR acc|RTL imp|JMP absind|ADC abs|ROR abs|ADC absl
BVS rel8|ADC dpindy|ADC dpind|ADC sriy|STZ dpx|ADC dpx|ROR dpx|ADC dpindly|SEI imp|ADC absy|PLY imp|TDC imp|JMP absindx|ADC absx|ROR absx|ADC abslx
BRA rel8|STA dpindx|BRL rel16|STA sr|STY dp|STA dp|STX dp|STA dpindl|DEY imp|BIT imm_m|TXA imp|PHB imp|STY abs|STA abs|STX abs|STA absl
BCC rel8|STA dpindy|STA dpind|STA sriy|STY dpx|STA dpx|STX dpy|STA dpindly|TYA imp|STA absy|TXS imp|TXY imp|STZ abs|STA absx|STZ absx|STA abslx
LDY imm_x|LDA dpindx|LDX imm_x|LDA sr|LDY dp|LDA dp|LDX dp|LDA dpindl|TAY imp|LDA imm_m|TAX imp|PLB imp|LDY abs|LDA abs|LDX abs|LDA absl
BCS rel8|LDA dpindy|LDA dpind|LDA sriy|LDY dpx|LDA dpx|LDX dpy|LDA dpindly|CLV imp|LDA absy|TSX imp|TYX imp|LDY absx|LDA absx|LDX absy|LDA abslx
CPY imm_x|CMP dpindx|REP imm8|CMP sr|CPY dp|CMP dp|DEC dp|CMP dpindl|INY imp|CMP imm_m|DEX imp|WAI imp|CPY abs|CMP abs|DEC abs|CMP absl
BNE rel8|CMP dpindy|CMP dpind|CMP sriy|PEI dpind|CMP dpx|DEC dpx|CMP dpindly|CLD imp|CMP absy|PHX imp|STP imp|JML absindl|CMP absx|DEC absx|CMP abslx
CPX imm_x|SBC dpindx|SEP imm8|SBC sr|CPX dp|SBC dp|INC dp|SBC dpindl|INX imp|SBC imm_m|NOP imp|XBA imp|CPX abs|SBC abs|INC abs|SBC absl
BEQ rel8|SBC dpindy|SBC dpind|SBC sriy|PEA imm16|SBC dpx|INC dpx|SBC dpindly|SED imp|SBC absy|PLX imp|XCE imp|JSR absindx|SBC absx|INC absx|SBC abslx
"""


class Opcode(NamedTuple):
    code: int
    mnemonic: str
    mode: str


OPCODES = [
    Opcode(i, *entry.split())
    for i, entry in enumerate(e.strip() for line in _TABLE.strip().splitlines() for e in line.split("|"))
]
assert len(OPCODES) == 256

BRANCHES = {"BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ"}
# Instructions after which execution never falls through to the next one.
TERMINATORS = {"RTS", "RTL", "RTI", "JMP", "JML", "BRA", "BRL", "STP", "BRK", "COP", "WDM"}
CALLS = {"JSR", "JSL"}


def length(op: Opcode, m8: Optional[bool], x8: Optional[bool]) -> Optional[int]:
    """Instruction length, or None when it depends on a flag whose value is unknown."""
    if op.mode == IMM_M:
        return None if m8 is None else (2 if m8 else 3)
    if op.mode == IMM_X:
        return None if x8 is None else (2 if x8 else 3)
    return 1 + _OPERAND_BYTES[op.mode]


def format_operand(op: Opcode, operand: bytes, pc: int) -> str:
    """Assembler syntax for the operand; branch targets are resolved against `pc`."""
    value = int.from_bytes(operand, "little")
    mode = op.mode
    if mode in (IMP,):
        return ""
    if mode == ACC:
        return "A"
    if mode in (IMM_M, IMM_X, IMM8, IMM16):
        return f"#${value:0{len(operand) * 2}X}"
    if mode == REL8:
        offset = value - 0x100 if value & 0x80 else value
        return f"${(pc & 0xFF0000) | ((pc + 2 + offset) & 0xFFFF):06X}"
    if mode == REL16:
        offset = value - 0x10000 if value & 0x8000 else value
        return f"${(pc & 0xFF0000) | ((pc + 3 + offset) & 0xFFFF):06X}"
    if mode == MOVE:
        return f"${operand[1]:02X},${operand[0]:02X}"
    width = len(operand) * 2
    template = {
        DP: "${}", DPX: "${},X", DPY: "${},Y", DPIND: "(${})", DPINDX: "(${},X)", DPINDY: "(${}),Y",
        DPINDL: "[${}]", DPINDLY: "[${}],Y", ABS: "${}", ABSX: "${},X", ABSY: "${},Y", ABSL: "${}",
        ABSLX: "${},X", ABSIND: "(${})", ABSINDX: "(${},X)", ABSINDL: "[${}]", SR: "${},S", SRIY: "(${},S),Y",
    }[mode]
    return template.format(f"{value:0{width}X}")
