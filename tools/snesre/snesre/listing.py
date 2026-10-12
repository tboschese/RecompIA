"""Writes the analysis as an assembly listing and a JSON database."""

from __future__ import annotations

import json
from collections import Counter
from pathlib import Path

from .analyze import Analysis


def write_listing(analysis: Analysis, path: Path) -> None:
    rom = analysis.rom
    lines = [f"; {rom.title} — {rom.mapping}, {len(rom.data) // 1024} KB, SHA-1 {rom.sha1}", ""]
    previous_end = None
    for offset in sorted(set(analysis.instructions) | set(analysis.tables)):
        if previous_end is not None and offset != previous_end:
            lines.append(f"; --- {offset - previous_end:+d} bytes não analisados ---" if offset > previous_end else "; --- sobreposição ---")
        table = analysis.tables.get(offset)
        if table is not None:
            lines.append("")
            lines.append(f"{analysis.labels.get(table.address, f'tbl_{table.address:06X}')}:  ; usada por {table.pc:06X}")
            for i, target in enumerate(table.targets):
                address = table.address + 2 * i
                name = analysis.labels.get(target, f"${target & 0xFFFF:04X}")
                lines.append(f"  {address >> 16:02X}:{address & 0xFFFF:04X}  {target & 0xFF:02X} {target >> 8 & 0xFF:02X}        .dw {name}")
            previous_end = offset + 2 * len(table.targets)
            continue
        ins = analysis.instructions[offset]
        if ins.pc in analysis.labels:
            lines.append("")
            lines.append(f"{analysis.labels[ins.pc]}:")
        lines.append(_line(ins, rom.data[offset:offset + ins.size]))
        previous_end = offset + ins.size

    if analysis.ram_blocks:
        lines += ["", "", "; ===== Código executado na RAM (do registro de execução) ====="]
    for block in analysis.ram_blocks:
        origin = f"copiado de {block.source >> 16:02X}:{block.source & 0xFFFF:04X}" if block.source is not None else "origem não encontrada na ROM"
        lines += ["", f"ram_{block.address:06X}:  ; {origin}"]
        for ins in block.instructions:
            lines.append(_line(ins, analysis.trace.wram_code[ins.offset:ins.offset + ins.size]))
    path.write_text("\n".join(lines) + "\n")


def _line(ins, raw_bytes) -> str:
    raw = " ".join(f"{b:02X}" for b in raw_bytes)
    flags = "".join(
        symbol if value else symbol.lower() if value is not None else "?"
        for symbol, value in (("M", ins.state.m8), ("X", ins.state.x8))
    )
    return f"  {ins.pc >> 16:02X}:{ins.pc & 0xFFFF:04X}  {raw:<12} {ins.text:<22} ; {flags}"


def write_database(analysis: Analysis, path: Path) -> None:
    path.write_text(json.dumps({
        "title": analysis.rom.title,
        "sha1": analysis.rom.sha1,
        "functions": sorted(f"{pc:06X}" for pc in analysis.functions),
        "labels": {f"{pc:06X}": name for pc, name in sorted(analysis.labels.items())},
        "jump_tables": [
            {"pc": f"{t.pc:06X}", "address": f"{t.address:06X}", "targets": [f"{x:06X}" for x in t.targets]}
            for _, t in sorted(analysis.tables.items())
        ],
        "ram_code": [
            {"address": f"{b.address:06X}", "instructions": len(b.instructions),
             "source": f"{b.source:06X}" if b.source is not None else None}
            for b in analysis.ram_blocks
        ],
        "issues": [{"pc": f"{i.pc:06X}", "kind": i.kind, "detail": i.detail} for i in analysis.issues],
    }, indent=1, ensure_ascii=False))


def summary(analysis: Analysis) -> str:
    rom = analysis.rom
    kinds = Counter(issue.kind for issue in analysis.issues)
    code = analysis.code_bytes
    lines = [
        f"{rom.title} ({rom.mapping}{', FastROM' if rom.fastrom else ''}) CRC32 {rom.crc32}",
        f"  instruções:   {len(analysis.instructions):,}",
        f"  funções:      {len(analysis.functions):,}",
        f"  tabelas:      {len(analysis.tables):,} tabelas de salto, {sum(len(t.targets) for t in analysis.tables.values()):,} destinos",
        f"  código achado: {code:,} bytes ({code * 100 / len(rom.data):.2f}% da ROM)",
    ]
    trace = analysis.trace
    if trace is not None:
        executed = {i for i, flags in enumerate(trace.rom) if flags & 0x03}
        found = set()
        for ins in analysis.instructions.values():
            found.update(range(ins.offset, ins.offset + ins.size))
        ram = sum(len(b.instructions) for b in analysis.ram_blocks)
        sourced = sum(1 for b in analysis.ram_blocks if b.source is not None)
        lines.append(f"  rastreamento: {len(executed):,} bytes executados; a análise cobre "
                     f"{len(executed & found) * 100 / max(len(executed), 1):.1f}% deles")
        lines.append(f"  código na RAM: {ram:,} instruções em {len(analysis.ram_blocks)} blocos, "
                     f"{sourced} com origem achada na ROM")
    lines.append("  pendências:   " + (", ".join(f"{kind} {count}" for kind, count in kinds.most_common()) or "nenhuma"))
    return "\n".join(lines)
