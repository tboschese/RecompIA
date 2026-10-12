from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import List, Optional

from . import __version__, enhance, toolchain
from .builder import BuildError, BuildOptions, build
from .consoles import CONSOLES, DetectionError, supported_extensions
from .cores import SUPPORTED_ABIS
from .roms import cue_tracks


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="rom2apk",
        description="Transforma ROMs de videogame em APKs Android jogáveis.",
    )
    parser.add_argument("--version", action="version", version=f"rom2apk {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    b = sub.add_parser("build", help="gera APKs a partir de ROMs")
    b.add_argument("rom", type=Path, nargs="+",
                   help="ROMs (.nes, .sfc, .gba, .cue, .z64, .zip…) ou pastas com ROMs")
    b.add_argument("--console", choices=sorted(CONSOLES), help="força o console (padrão: detecta pela extensão)")
    b.add_argument("--name", help="nome do app (padrão: nome do arquivo)")
    b.add_argument("--package", help="identificador do app, ex.: com.seunome.meujogo")
    b.add_argument("--icon", type=Path, help="ícone PNG/WEBP/JPG (quadrado, 512x512 recomendado)")
    b.add_argument("--patch", type=Path,
                   help="patch IPS/BPS aplicado à ROM (traduções, hacks MSU-1…)")
    b.add_argument("--no-auto-icon", dest="auto_icon", action="store_false",
                   help="não cria o ícone a partir da tela do jogo (usa o ícone padrão)")
    b.add_argument("--video", choices=enhance.VIDEO_PROFILES, default="hd",
                   help="perfil de imagem inicial (padrão: hd)")
    b.add_argument("--mode7", choices=list(enhance.SNES_MODE7), default="4x",
                   help="SNES: resolução do Mode 7 (F-Zero, Mario Kart, Pilotwings…). Padrão: 4x")
    b.add_argument("--aspect", choices=list(enhance.SNES_ASPECTS), default="4:3",
                   help="SNES: proporção da tela — 4:3 (como na TV) ou 8:7 (pixels quadrados). Padrão: 4:3")
    b.add_argument("--scale", type=int, choices=[1, 2, 3, 4],
                   help="resolução interna para PS1/N64 (PS1 até 2x, N64 até 4x)")
    b.add_argument("--bios", type=Path, action="append", default=[],
                   help="arquivo de BIOS a incluir (pode repetir)")
    b.add_argument("--abi", action="append", choices=SUPPORTED_ABIS,
                   help="arquitetura alvo (padrão: arm64-v8a; pode repetir)")
    b.add_argument("--out", type=Path, default=Path("dist"), help="pasta de saída (padrão: ./dist)")
    b.add_argument("--version-code", type=int, default=1, help="versionCode do APK (aumente para atualizar)")
    b.add_argument("--keystore", help="keystore próprio para assinar o APK")
    b.add_argument("--keystore-pass", help="senha do keystore")
    b.add_argument("--key-alias", help="alias da chave no keystore")
    b.add_argument("--key-pass", help="senha da chave (padrão: a mesma do keystore)")
    b.add_argument("--refresh-cores", action="store_true", help="baixa novamente os núcleos de emulação")
    b.add_argument("-v", "--verbose", action="store_true", help="mostra a saída completa do Gradle")

    sub.add_parser("consoles", help="lista os consoles suportados")
    sub.add_parser("doctor", help="verifica se o ambiente está pronto para gerar APKs")

    args = parser.parse_args(argv)
    try:
        if args.command == "consoles":
            return _consoles()
        if args.command == "doctor":
            return _doctor()
        return _build(args)
    except (BuildError, DetectionError, toolchain.ToolchainError) as error:
        print(f"erro: {error}", file=sys.stderr)
        return 1


def _collect_roms(paths: List[Path]) -> List[Path]:
    known = supported_extensions() | {".zip"}
    roms: List[Path] = []
    for path in paths:
        if not path.exists():
            raise BuildError(f"Arquivo não encontrado: {path}")
        if path.is_file():
            roms.append(path)
            continue
        files = sorted(
            p for p in path.rglob("*")
            if p.is_file() and not p.name.startswith(".") and p.suffix.lower() in known
        )
        # Tracks referenced by a cue sheet are built through the .cue, not on their own.
        tracks = set()
        for cue in (p for p in files if p.suffix.lower() == ".cue"):
            tracks.update(t.resolve() for t in cue_tracks(cue))
        roms.extend(p for p in files if p.resolve() not in tracks)
    if not roms:
        raise BuildError("Nenhuma ROM encontrada.")
    return roms


def _build(args: argparse.Namespace) -> int:
    roms = _collect_roms(args.rom)
    if len(roms) > 1 and (args.name or args.package or args.icon or args.patch):
        raise BuildError("--name, --package, --icon e --patch só podem ser usados com uma única ROM.")
    for extra in [args.icon, args.patch, *args.bios]:
        if extra and not extra.exists():
            raise BuildError(f"Arquivo não encontrado: {extra}")

    keystore = None
    if args.keystore:
        if not (args.keystore_pass and args.key_alias):
            raise BuildError("--keystore exige --keystore-pass e --key-alias")
        keystore = toolchain.custom_keystore(args.keystore, args.keystore_pass, args.key_alias, args.key_pass)

    failures = []
    for index, rom in enumerate(roms, start=1):
        if len(roms) > 1:
            print(f"\n[{index}/{len(roms)}] {rom.name}", file=sys.stderr)
        try:
            apk = build(BuildOptions(
                rom=rom.resolve(),
                out_dir=args.out.resolve(),
                console=args.console,
                title=args.name,
                package=args.package,
                icon=args.icon.resolve() if args.icon else None,
                video=args.video,
                scale=args.scale,
                mode7=args.mode7,
                auto_icon=args.auto_icon,
                patch=args.patch.resolve() if args.patch else None,
                aspect=args.aspect,
                bios=[p.resolve() for p in args.bios],
                abis=args.abi or ["arm64-v8a"],
                keystore=keystore,
                refresh_cores=args.refresh_cores,
                version_code=args.version_code,
                verbose=args.verbose,
            ))
        except (BuildError, DetectionError) as error:
            if len(roms) == 1:
                raise
            print(f"erro: {error}", file=sys.stderr)
            failures.append(rom.name)
            continue
        size_mb = apk.stat().st_size / 1e6
        print(f"APK gerado: {apk} ({size_mb:.1f} MB)")

    if len(roms) > 1:
        print(f"\n{len(roms) - len(failures)} de {len(roms)} APKs gerados.", file=sys.stderr)
        if failures:
            print("Falharam: " + ", ".join(failures), file=sys.stderr)
    return 1 if failures else 0


def _consoles() -> int:
    for console in CONSOLES.values():
        extensions = " ".join(console.extensions)
        extra = f"  (upscaling até {enhance.MAX_SCALE[console.id]}x)" if console.is_3d else ""
        print(f"{console.id:<5} {console.name:<22} {console.core:<24} {extensions}{extra}")
    return 0


def _doctor() -> int:
    ok = True
    try:
        java_home = toolchain.find_java_home()
        print(f"✔ JDK: {java_home}")
    except toolchain.ToolchainError as error:
        print(f"✘ {error}")
        ok = False
    try:
        sdk = toolchain.find_android_sdk()
        print(f"✔ Android SDK: {sdk}")
        for problem in toolchain.check(sdk):
            print(f"✘ {problem}")
            ok = False
    except toolchain.ToolchainError as error:
        print(f"✘ {error}")
        ok = False
    print("Tudo pronto!" if ok else "Corrija os itens acima antes de gerar APKs.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
