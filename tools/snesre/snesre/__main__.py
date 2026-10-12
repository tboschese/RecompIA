"""snesre: engenharia reversa de jogos de SNES.

    python3 -m snesre analyze games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import listing, rom as rom_module
from .analyze import State, analyze


def main() -> int:
    parser = argparse.ArgumentParser(prog="snesre", description="Engenharia reversa de jogos de SNES.")
    sub = parser.add_subparsers(dest="command", required=True)
    a = sub.add_parser("analyze", help="descobre o código do jogo e gera a listagem")
    a.add_argument("game", type=Path, help="game.json do projeto (em games/<jogo>/)")
    a.add_argument("rom", type=Path, help="a sua ROM do jogo")
    a.add_argument("--out", type=Path, help="pasta de saída (padrão: build/<jogo>)")
    args = parser.parse_args()

    game = json.loads(args.game.read_text())
    rom = rom_module.load(args.rom)
    if game.get("sha1") and rom.sha1 != game["sha1"]:
        print(f"erro: esta ROM não é a versão esperada ({game['name']}).\n"
              f"  esperado SHA-1 {game['sha1']}\n  recebido SHA-1 {rom.sha1}", file=sys.stderr)
        return 1

    entries = []
    for name, address in rom.vectors().items():
        state = State(True, True, True) if name == "reset" or name.startswith("emu_") else State(None, None, False)
        entries.append((address, state, name))
    for entry in game.get("entries", []):
        state = State(entry.get("m8"), entry.get("x8"), False)
        entries.append((int(entry["address"], 16), state, entry["name"]))

    analysis = analyze(rom, entries)
    for address, name in game.get("labels", {}).items():
        analysis.labels[int(address, 16)] = name

    out = args.out or Path("build") / args.game.parent.name
    out.mkdir(parents=True, exist_ok=True)
    listing.write_listing(analysis, out / "disassembly.asm")
    listing.write_database(analysis, out / "analysis.json")
    print(listing.summary(analysis))
    print(f"  saída:        {out}/disassembly.asm, {out}/analysis.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
