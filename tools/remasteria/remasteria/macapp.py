"""Packages a game project as a macOS app: engine + core + SDL2 + gamepad database + the user's ROM + icon."""

from __future__ import annotations

import json
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from . import TOOLS, cores
from snesre import rom as rom_module, tracer

ICON_STYLE = 1  # bump when icon.py changes so cached icons are regenerated
ICON_SIZES = (16, 32, 128, 256, 512)


def _run(*cmd) -> str:
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"falhou: {' '.join(map(str, cmd))}\n{result.stdout[-2000:]}")
    return result.stdout


def build(game_dir: Path, rom_path: Path, dist: Path) -> Path:
    game = json.loads((game_dir / "game.json").read_text())
    app = game["app"]
    rom = rom_module.load(rom_path)
    if game.get("sha1") and rom.sha1 != game["sha1"]:
        raise RuntimeError(f"esta ROM não é a versão esperada ({game['name']}): SHA-1 {rom.sha1}")

    core, engine = cores.core_path(app.get("core", "snes9x")), tracer.engine_path()
    bundle = dist / f"{app['title']}.app"
    if bundle.exists():
        shutil.rmtree(bundle)
    contents = bundle / "Contents"
    macos, resources, frameworks = contents / "MacOS", contents / "Resources", contents / "Frameworks"
    for folder in (macos, resources, frameworks):
        folder.mkdir(parents=True)

    executable = macos / app["id"]
    shutil.copy(engine, executable)
    shutil.copy(core, resources / "core.dylib")
    (resources / "game.sfc").write_bytes(rom.data)
    shutil.copytree(tracer.ENGINE / "shaders", resources / "shaders")
    shutil.copy(cores.controller_db_path(), resources / "gamecontrollerdb.txt")
    config = [f"title={app['title']}", f"video={app.get('video', 'pixel')}"]
    if app.get("remaster"):
        config.append("remaster=on")
    if app.get("art_capture", False):
        config.append("art_capture=on")
    lights = game_dir / app.get("lights", "lights.txt")
    if lights.exists():
        shutil.copy(lights, resources / "lights.txt")
        config.append("lights=lights.txt")
    if app.get("light_detect") is False:
        config.append("light_detect=off")
    if app.get("light_capture", False):
        config.append("light_capture=on")
    for style in app.get("art_styles") or []:
        pack = build_dir(app) / f"hdpack-{style}.bin"
        if pack.exists():
            shutil.copy(pack, resources / pack.name)
    if app.get("art_styles"):
        config.append("art_styles=" + ",".join(app["art_styles"]))
    config += [f"{key}={value}" for key, value in app.get("options", {}).items()]
    (resources / "game.cfg").write_text("\n".join(config) + "\n")
    _bundle_sdl(executable, frameworks)
    _icon(tracer.core_path(), rom_path, rom.sha1, float(app.get("icon_seconds", 0)), resources / "AppIcon.icns")

    with (contents / "Info.plist").open("wb") as f:
        plistlib.dump({
            "CFBundleName": app["title"],
            "CFBundleDisplayName": app["title"],
            "CFBundleIdentifier": f"com.remasteria.{app['id']}",
            "CFBundleExecutable": app["id"],
            "CFBundleIconFile": "AppIcon",
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": app.get("version", "0.1.0"),
            "CFBundleVersion": app.get("version", "0.1.0"),
            "LSMinimumSystemVersion": "11.0",
            "LSApplicationCategoryType": "public.app-category.role-playing-games",
            "NSHighResolutionCapable": True,
        }, f)

    # Copied and rewritten binaries must be signed again (ad hoc) to run on Apple Silicon.
    for binary in (*sorted(frameworks.glob("*.dylib")), resources / "core.dylib", executable):
        _run("codesign", "--force", "--sign", "-", str(binary))
    _run("codesign", "--force", "--sign", "-", str(bundle))
    return bundle


def build_dir(app: dict) -> Path:
    """Where a game's derived files live (ignored by Git): build/<id>."""
    return TOOLS.parent / "build" / app["id"]


def _bundle_sdl(executable: Path, frameworks: Path) -> None:
    """Ships the SDL2 library inside the app, so it runs without Homebrew."""
    linked = [line.split()[0] for line in _run("otool", "-L", str(executable)).splitlines()[1:]]
    sdl = next((path for path in linked if "libSDL2" in path), None)
    if sdl is None:
        raise RuntimeError("o motor não está ligado ao SDL2")
    target = frameworks / "libSDL2.dylib"
    shutil.copy(Path(sdl).resolve(), target)
    target.chmod(0o755)
    _run("install_name_tool", "-id", "@rpath/libSDL2.dylib", str(target))
    _run("install_name_tool", "-change", sdl, "@rpath/libSDL2.dylib", str(executable))
    _run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", str(executable))

    # Homebrew's SDL2 is sdl2-compat, which loads SDL3 from its own folder at runtime.
    if b"libSDL3.dylib" in target.read_bytes():
        candidates = [Path(sdl).resolve().parent / "libSDL3.dylib"]
        if shutil.which("brew"):
            prefix = subprocess.run(["brew", "--prefix", "sdl3"], stdout=subprocess.PIPE, text=True).stdout.strip()
            candidates.append(Path(prefix) / "lib" / "libSDL3.dylib")
        sdl3 = next((path for path in candidates if path.exists()), None)
        if sdl3 is None:
            raise RuntimeError("o SDL2 instalado depende do SDL3, que não foi encontrado (brew install sdl3)")
        shutil.copy(sdl3.resolve(), frameworks / "libSDL3.dylib")
        (frameworks / "libSDL3.dylib").chmod(0o755)
        _run("install_name_tool", "-id", "@rpath/libSDL3.dylib", str(frameworks / "libSDL3.dylib"))


def _icon(core: Path, rom_path: Path, sha1: str, seconds: float, target: Path) -> None:
    cached = tracer.home() / "icons" / f"{sha1}-{seconds:g}s-mac{ICON_STYLE}.png"
    if not cached.exists():
        print("Capturando uma cena do jogo para o ícone…", file=sys.stderr)
        cached.parent.mkdir(parents=True, exist_ok=True)
        partial = cached.with_suffix(".part.png")
        subprocess.run([sys.executable, "-m", "remasteria.icon", str(core), str(rom_path.resolve()), f"{seconds:g}", str(partial)],
                       cwd=TOOLS / "remasteria", check=True, timeout=180)
        partial.replace(cached)

    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "AppIcon.iconset"
        iconset.mkdir()
        for size in ICON_SIZES:
            for scale in (1, 2):
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                pixels = size * scale
                _run("sips", "-z", str(pixels), str(pixels), str(cached), "--out", str(iconset / name))
        _run("iconutil", "-c", "icns", str(iconset), "-o", str(target))
