"""Hardware cores the apps can ship, built from source and cached in ~/.snesre/cores.

- snes9x: snes9x with snesre's execution tracer (playing the app feeds the analysis).
- bsnes-hd: bsnes-hd beta, for sharp HD Mode 7 and widescreen.

Also the community gamepad database (SDL_GameControllerDB, zlib license), so USB and Bluetooth
gamepads SDL doesn't know by itself get the SNES layout out of the box.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

from . import TOOLS  # noqa: F401  (puts snesre on the path)
from snesre import tracer

BSNES_HD_REPO = "https://github.com/DerKoun/bsnes-hd.git"
BSNES_HD_COMMIT = "fc26b25ea236f0f877f0265d2a2c37dfd93dfde9"
CONTROLLER_DB_COMMIT = "c1d5289a1f713b30a2c121e9fe6529d39360be0f"
CONTROLLER_DB_URL = f"https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/{CONTROLLER_DB_COMMIT}/gamecontrollerdb.txt"


def _run(cmd, cwd: Path) -> None:
    result = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"falhou: {' '.join(map(str, cmd))}\n{result.stdout[-4000:]}")


def bsnes_hd_path() -> Path:
    ext = "dylib" if platform.system() == "Darwin" else "so"
    target = tracer.home() / "cores" / f"bsnes_hd-{BSNES_HD_COMMIT[:12]}.{ext}"
    if target.exists():
        return target
    print("Compilando o núcleo bsnes-hd (só na primeira vez)…", file=sys.stderr)
    src = tracer.home() / "build" / f"bsnes-hd-{BSNES_HD_COMMIT[:12]}"
    if src.exists():
        shutil.rmtree(src)
    src.mkdir(parents=True)
    _run(["git", "init", "-q"], src)
    _run(["git", "fetch", "-q", "--depth", "1", BSNES_HD_REPO, BSNES_HD_COMMIT], src)
    _run(["git", "checkout", "-q", "FETCH_HEAD"], src)
    _run(["make", f"-j{os.cpu_count() or 4}", "target=libretro", "binary=library"], src / "bsnes")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(src / "bsnes" / "out" / f"bsnes_hd_beta_libretro.{ext}", target)
    shutil.rmtree(src)
    return target


def core_path(name: str) -> Path:
    if name == "snes9x":
        return tracer.core_path()
    if name == "bsnes-hd":
        return bsnes_hd_path()
    raise RuntimeError(f"núcleo desconhecido: {name} (use snes9x ou bsnes-hd)")


def controller_db_path() -> Path:
    target = tracer.home() / "cores" / f"gamecontrollerdb-{CONTROLLER_DB_COMMIT[:12]}.txt"
    if not target.exists():
        print("Baixando a base de controles USB…", file=sys.stderr)
        target.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(CONTROLLER_DB_URL, timeout=60) as response:
            data = response.read()
        partial = target.with_suffix(".part")
        partial.write_bytes(data)
        partial.replace(target)
    return target
