"""snesre: engenharia reversa de jogos de SNES.

    python3 -m snesre play    games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"
    python3 -m snesre analyze games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"

`play` abre o jogo num player que registra todo o código que roda (build/<jogo>/trace.bin);
`analyze` junta esse registro (e qualquer .cdl do Mesen 2 em build/<jogo>/) à análise estática.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import listing, rom as rom_module, trace as trace_module, tracer
from .analyze import State, analyze


def _load(args) -> tuple:
    game = json.loads(args.game.read_text())
    rom = rom_module.load(args.rom)
    if game.get("sha1") and rom.sha1 != game["sha1"]:
        raise SystemExit(f"erro: esta ROM não é a versão esperada ({game['name']}).\n"
                         f"  esperado SHA-1 {game['sha1']}\n  recebido SHA-1 {rom.sha1}")
    out = args.out or Path("build") / args.game.parent.name
    return game, rom, out


def play(args) -> int:
    _, _, out = _load(args)
    if args.frames:
        return tracer.play(args.rom, out / "trace.bin", out / "saves", args.frames)
    print("Controles: setas, Z=B X=A A=Y S=X Q=L W=R, Enter=Start, Shift direito=Select,\n"
          "           Tab (segurar)=acelerar, F5=salvar estado, F7=carregar estado, Esc=sair.\n"
          "Controles USB/Bluetooth funcionam no layout do SNES. Cada sessão soma ao registro.")
    return tracer.play(args.rom, out / "trace.bin", out / "saves")


def run_analysis(args) -> int:
    game, rom, out = _load(args)

    entries = []
    for name, address in rom.vectors().items():
        state = State(True, True, True) if name == "reset" or name.startswith("emu_") else State(None, None, False)
        entries.append((address, state, name))
    for entry in game.get("entries", []):
        state = State(entry.get("m8"), entry.get("x8"), False)
        entries.append((int(entry["address"], 16), state, entry["name"]))

    paths = list(args.trace or [])
    if not args.trace:
        candidates = [out / "trace.bin", *sorted(out.glob("*.cdl"))]
        if "app" in game:
            candidates.append(Path.home() / "Library" / "Application Support" / "RemakeIA" / game["app"]["title"] / "trace.bin")
        paths += [p for p in candidates if p.exists()]
    trace = None
    for path in paths:
        loaded = trace_module.load(path, len(rom.data))
        if trace is None:
            trace = loaded
        else:
            trace.merge(loaded)
        print(f"  usando registro de execução: {path}")

    analysis = analyze(rom, entries, trace)
    for address, name in game.get("labels", {}).items():
        analysis.labels[int(address, 16)] = name

    out.mkdir(parents=True, exist_ok=True)
    listing.write_listing(analysis, out / "disassembly.asm")
    listing.write_database(analysis, out / "analysis.json")
    print(listing.summary(analysis))
    print(f"  saída:        {out}/disassembly.asm, {out}/analysis.json")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(prog="snesre", description="Engenharia reversa de jogos de SNES.")
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p):
        p.add_argument("game", type=Path, help="game.json do projeto (em games/<jogo>/)")
        p.add_argument("rom", type=Path, help="a sua ROM do jogo")
        p.add_argument("--out", type=Path, help="pasta de saída (padrão: build/<jogo>)")

    a = sub.add_parser("analyze", help="descobre o código do jogo e gera a listagem")
    common(a)
    a.add_argument("--trace", type=Path, action="append",
                   help="registro de execução (trace.bin do snesre ou .cdl do Mesen 2); pode repetir")
    a.set_defaults(func=run_analysis)

    p = sub.add_parser("play", help="joga registrando o código que roda, para a análise")
    common(p)
    p.add_argument("--frames", type=int, help="roda sem janela por N quadros (registra abertura e título)")
    p.set_defaults(func=play)

    args = parser.parse_args()
    try:
        return args.func(args)
    except (RuntimeError, ValueError) as error:
        print(f"erro: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
