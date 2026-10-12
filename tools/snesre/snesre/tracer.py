"""Builds and runs the tracing player: snes9x (libretro) with snesre's tracer, run by the RemakeIA engine.

Everything is built once from source and cached in ~/.snesre, keyed by the snes9x commit and a hash
of our sources, so editing the tracer or the engine rebuilds it automatically.
"""

from __future__ import annotations

import hashlib
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

SNES9X_REPO = "https://github.com/libretro/snes9x.git"
SNES9X_COMMIT = "fae2fea08f74180759ef540ee94259213f503480"
SOURCES = Path(__file__).resolve().parent.parent / "tracer"
ENGINE = Path(__file__).resolve().parents[3] / "engine"
ENGINE_SOURCES = ("src/main.c", "src/video.c", "src/video.h", "src/input.c", "src/input.h", "src/menu.c", "src/menu.h",
                  "src/third_party/font8x8_basic.h", "src/third_party/font8x8_ext_latin.h")
CORE_FILES = ("snesre_trace.cpp", "snesre_trace.h")


def home() -> Path:
    return Path(os.environ.get("SNESRE_HOME", Path.home() / ".snesre"))


def _digest(*paths) -> str:
    h = hashlib.sha256()
    for path in paths:
        h.update((SOURCES / path).read_bytes())
    return h.hexdigest()[:12]


def _run(cmd, cwd: Path = None) -> None:
    result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"falhou: {' '.join(map(str, cmd))}\n{result.stdout[-4000:]}")


def core_path() -> Path:
    ext = "dylib" if platform.system() == "Darwin" else "so"
    key = f"{SNES9X_COMMIT[:12]}-{_digest('snes9x.patch', *CORE_FILES)}"
    target = home() / "cores" / f"snes9x_snesre-{key}.{ext}"
    if target.exists():
        return target

    print("Compilando o núcleo snes9x com rastreamento (só na primeira vez)…", file=sys.stderr)
    src = home() / "build" / f"snes9x-{key}"
    if src.exists():
        shutil.rmtree(src)
    src.mkdir(parents=True)
    _run(["git", "init", "-q"], src)
    _run(["git", "fetch", "-q", "--depth", "1", SNES9X_REPO, SNES9X_COMMIT], src)
    _run(["git", "checkout", "-q", "FETCH_HEAD"], src)
    _run(["git", "apply", str(SOURCES / "snes9x.patch")], src)
    for name in CORE_FILES:
        shutil.copy(SOURCES / name, src / name)
    _run(["make", f"-j{os.cpu_count() or 4}"], src / "libretro")

    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(src / "libretro" / f"snes9x_libretro.{ext}", target)
    shutil.rmtree(src)
    return target


def engine_path() -> Path:
    sources = [ENGINE / name for name in ENGINE_SOURCES]
    target = home() / "bin" / f"engine-{_digest(*sources)}"
    if target.exists():
        return target
    if not shutil.which("sdl2-config"):
        raise RuntimeError("SDL2 não encontrado. No macOS: brew install sdl2")
    print("Compilando o motor…", file=sys.stderr)
    flags = subprocess.run(["sdl2-config", "--cflags", "--libs"], stdout=subprocess.PIPE, text=True, check=True).stdout.split()
    if platform.system() == "Darwin":
        flags += ["-framework", "OpenGL"]
    else:
        flags += ["-lGL"]
    target.parent.mkdir(parents=True, exist_ok=True)
    _run(["cc", "-O2", "-std=c11", "-Wall", "-o", str(target), *(str(p) for p in sources if p.suffix == ".c"), *flags])
    return target


def play(rom: Path, trace: Path, saves: Path, frames: Optional[int] = None) -> int:
    """Opens the game in a window; with `frames`, runs headless for that many frames instead."""
    core, player = core_path(), engine_path()
    saves.mkdir(parents=True, exist_ok=True)
    trace.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SNESRE_TRACE=str(trace.resolve()), REMAKEIA_SHADERS=str(ENGINE / "shaders"))
    cmd = [str(player), str(core), str(rom.resolve()), str(saves.resolve())]
    if frames:
        cmd.append(str(frames))
    return subprocess.call(cmd, env=env)
