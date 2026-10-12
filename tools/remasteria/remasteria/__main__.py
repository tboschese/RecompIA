"""RemasterIA: gera os aplicativos dos projetos de jogos.

    PYTHONPATH=tools/remasteria python3 -m remasteria mac games/chrono-trigger "Chrono Trigger (USA).sfc"
    PYTHONPATH=tools/remasteria python3 -m remasteria hdpack games/chrono-trigger

`hdpack` redesenha com IA a arte que o app capturou enquanto o jogo era jogado e gera
build/<jogo>/hdpack.bin, que o `mac` coloca dentro do app.
"""

from __future__ import annotations

import argparse
import json
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
    hd = sub.add_parser("hdpack", help="redesenha com IA a arte capturada e gera o pacote HD")
    hd.add_argument("game", type=Path, help="pasta do projeto (games/<jogo>)")
    hd.add_argument("captures", type=Path, nargs="*",
                    help="pastas de captura (padrão: a do app, ~/Library/Application Support/RemasterIA/<título>/arte)")
    hd.add_argument("--upscaler", choices=("esrgan", "nearest"), default="esrgan",
                    help="esrgan: IA Real-ESRGAN (padrão); nearest: só amplia, para testar o caminho")
    args = parser.parse_args()

    try:
        if args.command == "mac":
            bundle = macapp.build(args.game, args.rom, args.dist)
            print(f"pronto: {bundle}\n  abra com: open \"{bundle}\"")
        else:
            from . import hdpack

            app = json.loads((args.game / "game.json").read_text())["app"]
            captures = args.captures or [Path.home() / "Library" / "Application Support" / "RemasterIA" / app["title"] / "arte"]
            build = macapp.build_dir(app)
            tiles = hdpack.build(captures, build / "hdpack.bin", build / f"arte-{args.upscaler}", args.upscaler)
            print(f"pronto: {tiles} tiles redesenhados em {build / 'hdpack.bin'}\n  gere o app de novo para incluí-los")
    except RuntimeError as error:
        print(f"erro: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
