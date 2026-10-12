from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import List, Optional

from . import __version__, enhance, toolchain
from .builder import BuildError, BuildOptions, build
from .consoles import CONSOLES, DetectionError
from .cores import SUPPORTED_ABIS


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="rom2apk",
        description="Transforma ROMs de videogame em APKs Android jogáveis.",
    )
    parser.add_argument("--version", action="version", version=f"rom2apk {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    b = sub.add_parser("build", help="gera um APK a partir de uma ROM")
    b.add_argument("rom", type=Path, help="arquivo da ROM (.nes, .sfc, .gba, .cue, .z64, .zip…)")
    b.add_argument("--console", choices=sorted(CONSOLES), help="força o console (padrão: detecta pela extensão)")
    b.add_argument("--name", help="nome do app (padrão: nome do arquivo)")
    b.add_argument("--package", help="identificador do app, ex.: com.seunome.meujogo")
    b.add_argument("--icon", type=Path, help="ícone PNG/WEBP/JPG (quadrado, 512x512 recomendado)")
    b.add_argument("--video", choices=enhance.VIDEO_PROFILES, default="hd",
                   help="perfil de imagem inicial (padrão: hd)")
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


def _build(args: argparse.Namespace) -> int:
    if not args.rom.exists():
        raise BuildError(f"Arquivo não encontrado: {args.rom}")
    for extra in [args.icon, *args.bios]:
        if extra and not extra.exists():
            raise BuildError(f"Arquivo não encontrado: {extra}")

    keystore = None
    if args.keystore:
        if not (args.keystore_pass and args.key_alias):
            raise BuildError("--keystore exige --keystore-pass e --key-alias")
        keystore = toolchain.custom_keystore(args.keystore, args.keystore_pass, args.key_alias, args.key_pass)

    apk = build(BuildOptions(
        rom=args.rom.resolve(),
        out_dir=args.out.resolve(),
        console=args.console,
        title=args.name,
        package=args.package,
        icon=args.icon.resolve() if args.icon else None,
        video=args.video,
        scale=args.scale,
        bios=[p.resolve() for p in args.bios],
        abis=args.abi or ["arm64-v8a"],
        keystore=keystore,
        refresh_cores=args.refresh_cores,
        version_code=args.version_code,
        verbose=args.verbose,
    ))
    size_mb = apk.stat().st_size / 1e6
    print(f"APK gerado: {apk} ({size_mb:.1f} MB)")
    return 0


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
