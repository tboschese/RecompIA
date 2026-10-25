"""Build the RecompIA macOS application."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(prog="recompia", description="RecompIA · ports nativos para macOS, jogo por jogo.")
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="compila a ferramenta RecompIA.app")
    build.add_argument("--dist", type=Path, default=Path("dist"), help="pasta de saída (padrão: dist)")
    args = parser.parse_args()
    try:
        from . import macapp
        bundle = macapp.build(args.dist)
        print(f'pronto: {bundle}\n  abra com: open "{bundle}"')
    except (RuntimeError, OSError) as error:
        print(f"erro: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
