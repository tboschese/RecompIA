"""RemasterIA: o software que gera o remaster de um jogo.

    PYTHONPATH=tools/remasteria python3 -m remasteria mac games/<jogo> caminho/da/rom
    PYTHONPATH=tools/remasteria python3 -m remasteria lights survey|review|decide|test games/<jogo> …

Cada jogo é uma pasta em games/. O comando confere a ROM, compila o motor e o núcleo,
e entrega um aplicativo. Se o projeto liga `remaster`, o motor redesenha a imagem
em pixels duros enquanto o jogo roda.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import macapp


def main() -> int:
    parser = argparse.ArgumentParser(prog="remasteria", description="Gera o remaster de um jogo a partir da pasta do projeto e da ROM.")
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
    li = sub.add_parser("lights", help="as luzes dinâmicas do jogo, por tile: levantar, revisar, decidir, testar")
    lsub = li.add_subparsers(dest="action", required=True)
    sv = lsub.add_parser("survey", help="o motor joga sozinho e salva cenas, estados e as luzes que a detecção acha")
    sv.add_argument("game", type=Path)
    sv.add_argument("rom", type=Path)
    sv.add_argument("--frames", type=int, default=36000, help="quadros a jogar (padrão: 36000, 10 minutos)")
    sv.add_argument("--every", type=int, default=240, help="uma cena a cada N quadros (padrão: 240)")
    sv.add_argument("--press", action="store_true", help="aperta Start e A para passar dos menus (padrão: assiste à abertura)")
    rv = lsub.add_parser("review", help="junta as luzes achadas no lights.txt como ? e desenha a página de revisão")
    rv.add_argument("game", type=Path)
    rv.add_argument("captures", type=Path, nargs="*", help="pastas de luzes (padrão: build/<id>/luzes e a do app)")
    dc = lsub.add_parser("decide", help="decide grupos: 12=flame 13=none 14-20=glow 21=? (a detecção decide) all=none")
    dc.add_argument("game", type=Path)
    dc.add_argument("choices", nargs="+")
    ts = lsub.add_parser("test", help="desenha cada cena salva com as luzes do jogo e compara com o teste anterior")
    ts.add_argument("game", type=Path)
    ts.add_argument("rom", type=Path)
    ts.add_argument("--states", type=Path, help="pasta de estados (padrão: build/<id>/levantamento)")
    ts.add_argument("--frame", type=int, default=150, help="quadro desenhado depois de carregar cada estado (padrão: 150; o tempo do remaster só sai depois de ~120)")
    ts.add_argument("--no-detect", action="store_true", help="só as luzes da lista, sem a detecção")
    args = parser.parse_args()

    try:
        if args.command == "mac":
            bundle = macapp.build(args.game, args.rom, args.dist)
            print(f"pronto: {bundle}\n  abra com: open \"{bundle}\"")
        elif args.command == "lights":
            return _lights(args)
        else:
            from . import hdpack

            app = json.loads((args.game / "game.json").read_text())["app"]
            captures = args.captures or [Path.home() / "Library" / "Application Support" / "RemasterIA" / app["title"] / "arte"]
            build = macapp.build_dir(app)
            styles = app.get("art_styles")
            if not styles:
                raise RuntimeError("este jogo ainda não tem remaster de arte: isso é decidido jogo a jogo")
            made = hdpack.build(captures, build, build / f"arte-{args.upscaler}", styles, args.upscaler)
            print("pronto:")
            for name, tiles, path in made:
                print(f"  {name}: {tiles} tiles em {path}")
            print("gere o app de novo para incluí-los")
    except RuntimeError as error:
        print(f"erro: {error}", file=sys.stderr)
        return 1
    return 0


def _lights(args) -> int:
    from . import lights

    if args.action == "survey":
        states, found = lights.survey(args.game, args.rom, args.frames, args.every, args.press)
        print(f"cenas e estados em {states}\nluzes achadas em {found}\nagora: remasteria lights review {args.game}")
    elif args.action == "review":
        page, added, pending = lights.review(args.game, args.captures or None)
        print(f"{added} grupos novos no {lights.lights_file(args.game)}; {pending} a decidir\n  abra: open \"{page}\"")
    elif args.action == "decide":
        print(f"{lights.decide(args.game, args.choices)} tiles decididos em {lights.lights_file(args.game)}")
    else:
        result = lights.test(args.game, args.rom, args.states, args.frame, not args.no_detect)
        print(f"teste em {result['run']}")
        if result["slowest"]:
            print(f"  remaster mais lento: {result['slowest'][1]:.1f} ms ({result['slowest'][0]})")
        print(f"  {len(result['changed'])} cenas mudaram desde o teste anterior" + (f": {' '.join(result['changed'])}" if result["changed"] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
