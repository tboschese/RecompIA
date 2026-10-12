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
    for offset in sorted(analysis.instructions):
        ins = analysis.instructions[offset]
        if previous_end is not None and offset != previous_end:
            lines.append(f"; --- {offset - previous_end:+d} bytes não analisados ---" if offset > previous_end else "; --- sobreposição ---")
        if ins.pc in analysis.labels:
            lines.append("")
            lines.append(f"{analysis.labels[ins.pc]}:")
        raw = " ".join(f"{b:02X}" for b in rom.data[offset:offset + ins.size])
        flags = "".join(
            symbol if value else symbol.lower() if value is not None else "?"
            for symbol, value in (("M", ins.state.m8), ("X", ins.state.x8))
        )
        lines.append(f"  {ins.pc >> 16:02X}:{ins.pc & 0xFFFF:04X}  {raw:<12} {ins.text:<22} ; {flags}")
        previous_end = offset + ins.size
    path.write_text("\n".join(lines) + "\n")


def write_database(analysis: Analysis, path: Path) -> None:
    path.write_text(json.dumps({
        "title": analysis.rom.title,
        "sha1": analysis.rom.sha1,
        "functions": sorted(f"{pc:06X}" for pc in analysis.functions),
        "labels": {f"{pc:06X}": name for pc, name in sorted(analysis.labels.items())},
        "issues": [{"pc": f"{i.pc:06X}", "kind": i.kind, "detail": i.detail} for i in analysis.issues],
    }, indent=1, ensure_ascii=False))


def summary(analysis: Analysis) -> str:
    rom = analysis.rom
    kinds = Counter(issue.kind for issue in analysis.issues)
    code = analysis.code_bytes
    return "\n".join([
        f"{rom.title} ({rom.mapping}{', FastROM' if rom.fastrom else ''}) CRC32 {rom.crc32}",
        f"  instruções:   {len(analysis.instructions):,}",
        f"  funções:      {len(analysis.functions):,}",
        f"  código achado: {code:,} bytes ({code * 100 / len(rom.data):.2f}% da ROM)",
        "  pendências:   " + (", ".join(f"{kind} {count}" for kind, count in kinds.most_common()) or "nenhuma"),
    ])
