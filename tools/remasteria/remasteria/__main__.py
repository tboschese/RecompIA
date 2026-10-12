"""RemasterIA: gera os aplicativos dos projetos de jogos.

    PYTHONPATH=tools/remasteria python3 -m remasteria mac games/chrono-trigger "Chrono Trigger (USA).sfc"
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import macapp


def main() -> int:
    parser = argparse.ArgumentParser(prog="remasteria", description="Gera os aplicativos dos projetos de jogos.")
    sub = parser.add_subparsers(dest="command", required=True)
    mac = sub.add_parser("mac", help="gera o aplicativo para macOS")
    mac.add_argument("game", type=Path, help="pasta do projeto (games/<jogo>)")
    mac.add_argument("rom", type=Path, help="a sua ROM do jogo")
    mac.add_argument("--dist", type=Path, default=Path("dist"), help="pasta de saída (padrão: dist)")
    args = parser.parse_args()

    try:
        bundle = macapp.build(args.game, args.rom, args.dist)
    except RuntimeError as error:
        print(f"erro: {error}", file=sys.stderr)
        return 1
    print(f"pronto: {bundle}\n  abra com: open \"{bundle}\"")
    return 0


if __name__ == "__main__":
    sys.exit(main())
