"""Builds a game APK: copies the template into a cached workspace, injects the game and runs Gradle."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

from . import enhance, roms, toolchain
from .cores import core_path
from .paths import TEMPLATE_DIR, home

_PACKAGE_RE = re.compile(r"^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+$")
_ICON_EXTENSIONS = (".png", ".webp", ".jpg", ".jpeg")


class BuildError(Exception):
    pass


@dataclass
class BuildOptions:
    rom: Path
    out_dir: Path
    console: Optional[str] = None
    title: Optional[str] = None
    package: Optional[str] = None
    icon: Optional[Path] = None
    video: str = "hd"
    scale: Optional[int] = None
    bios: List[Path] = field(default_factory=list)
    abis: List[str] = field(default_factory=lambda: ["arm64-v8a"])
    keystore: Optional[toolchain.Keystore] = None
    refresh_cores: bool = False
    version_code: int = 1
    verbose: bool = False


def build(opts: BuildOptions) -> Path:
    sdk = toolchain.find_android_sdk()
    problems = toolchain.check(sdk)
    if problems:
        raise BuildError("\n".join(problems))
    java_home = toolchain.find_java_home()

    if opts.icon and opts.icon.suffix.lower() not in _ICON_EXTENSIONS:
        raise BuildError(f"Ícone precisa ser {', '.join(_ICON_EXTENSIONS)}")

    game = roms.prepare(opts.rom, opts.console, opts.title)
    try:
        console = game.console
        package = opts.package or f"com.rom2apk.{console.id}.{roms.slugify(game.title)}"
        if not _PACKAGE_RE.match(package):
            raise BuildError(f"Nome de pacote inválido: {package}")

        scale = enhance.resolve_scale(console, opts.scale)
        variables = enhance.core_variables(console, opts.video, scale)
        cores = {abi: core_path(console, abi, opts.refresh_cores) for abi in opts.abis}

        print(f"Console: {console.name} · núcleo {console.core} · vídeo {opts.video}"
              + (f" · resolução {scale}x" if console.is_3d else ""), file=sys.stderr)
        print(f"Pacote: {package}", file=sys.stderr)

        workspace = _sync_workspace()
        main = workspace / "app" / "src" / "main"

        roms.copy_files(game, main / "assets" / "game")
        for bios in opts.bios:
            (main / "assets" / "system").mkdir(parents=True, exist_ok=True)
            shutil.copyfile(bios, main / "assets" / "system" / bios.name)

        for abi, library in cores.items():
            target = main / "jniLibs" / abi
            target.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(library, target / console.core_library)

        (main / "assets" / "game.json").write_text(json.dumps({
            "title": game.title,
            "console": console.id,
            "core": console.core_library,
            "files": list(game.files),
            "main": game.main,
            "shader": enhance.initial_shader(console, opts.video),
            "variables": variables,
            "version": game.version,
        }, ensure_ascii=False, indent=2))

        _write_app_name(main, game.title)
        if opts.icon:
            _install_icon(main, opts.icon)

        (workspace / "local.properties").write_text(f"sdk.dir={sdk}\n")
        keystore = opts.keystore or toolchain.default_keystore(java_home)
        _run_gradle(workspace, sdk, java_home, {
            "rom2apk.applicationId": package,
            "rom2apk.abis": ",".join(opts.abis),
            "rom2apk.versionCode": str(opts.version_code),
            "rom2apk.storeFile": str(keystore.path),
            "rom2apk.storePassword": keystore.store_password,
            "rom2apk.keyAlias": keystore.alias,
            "rom2apk.keyPassword": keystore.key_password,
        }, opts.verbose)

        apk = workspace / "app" / "build" / "outputs" / "apk" / "release" / "app-release.apk"
        if not apk.exists():
            raise BuildError("O Gradle terminou sem gerar o APK.")
        opts.out_dir.mkdir(parents=True, exist_ok=True)
        output = opts.out_dir / f"{_safe_filename(game.title)}.apk"
        shutil.copyfile(apk, output)
        return output
    finally:
        game.cleanup()


def _sync_workspace() -> Path:
    """Refreshes sources from the template while keeping Gradle's incremental build caches."""
    workspace = home() / "workspace"
    workspace.mkdir(parents=True, exist_ok=True)
    for name in ("settings.gradle.kts", "build.gradle.kts", "gradle.properties", "gradlew", "gradlew.bat"):
        shutil.copy2(TEMPLATE_DIR / name, workspace / name)
    shutil.copytree(TEMPLATE_DIR / "gradle", workspace / "gradle", dirs_exist_ok=True)

    app = workspace / "app"
    app.mkdir(exist_ok=True)
    shutil.copy2(TEMPLATE_DIR / "app" / "build.gradle.kts", app / "build.gradle.kts")
    shutil.rmtree(app / "src", ignore_errors=True)
    shutil.copytree(TEMPLATE_DIR / "app" / "src", app / "src")
    (workspace / "gradlew").chmod(0o755)
    return workspace


def _write_app_name(main: Path, title: str) -> None:
    escaped = (
        title.replace("\\", "\\\\")
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace("'", "\\'")
        .replace('"', '\\"')
    )
    if escaped[:1] in ("@", "?"):
        escaped = "\\" + escaped
    (main / "res" / "values" / "strings.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        f'<resources>\n    <string name="app_name">{escaped}</string>\n</resources>\n',
        encoding="utf-8",
    )


def _install_icon(main: Path, icon: Path) -> None:
    (main / "res" / "mipmap-anydpi-v26" / "ic_launcher.xml").unlink()
    target_dir = main / "res" / "mipmap-nodpi"
    target_dir.mkdir(parents=True, exist_ok=True)
    ext = ".jpg" if icon.suffix.lower() == ".jpeg" else icon.suffix.lower()
    shutil.copyfile(icon, target_dir / f"ic_launcher{ext}")


def _run_gradle(workspace: Path, sdk: Path, java_home: Path, props: Dict[str, str], verbose: bool) -> None:
    command = [str(workspace / "gradlew"), "assembleRelease", "--console=plain"]
    command += [f"-P{key}={value}" for key, value in props.items()]
    env = {**os.environ, "JAVA_HOME": str(java_home), "ANDROID_HOME": str(sdk)}

    print("Compilando APK (a primeira vez demora alguns minutos)…", file=sys.stderr)
    if verbose:
        result = subprocess.run(command, cwd=workspace, env=env)
        output = ""
    else:
        result = subprocess.run(command, cwd=workspace, env=env, capture_output=True, text=True)
        output = result.stdout + result.stderr
    if result.returncode != 0:
        tail = "\n".join(output.strip().splitlines()[-40:])
        raise BuildError(f"Falha no Gradle (código {result.returncode}).\n{tail}")


def _safe_filename(title: str) -> str:
    cleaned = re.sub(r'[\\/:*?"<>|]+', "", title).strip()
    return cleaned or "game"
