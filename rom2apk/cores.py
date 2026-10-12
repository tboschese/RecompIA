"""Downloads libretro cores from the official buildbot and caches them locally."""

from __future__ import annotations

import io
import platform
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

from .consoles import Console
from .paths import home

BUILDBOT_URL = "https://buildbot.libretro.com/nightly/android/latest/{abi}/{core}_libretro_android.so.zip"
DESKTOP_URL = "https://buildbot.libretro.com/nightly/{platform}/latest/{core}_libretro.{ext}.zip"
SUPPORTED_ABIS = ("arm64-v8a", "armeabi-v7a", "x86_64", "x86")


def core_path(console: Console, abi: str, refresh: bool = False) -> Path:
    target = home() / "cores" / abi / console.core_library
    if target.exists() and not refresh:
        return target

    url = BUILDBOT_URL.format(abi=abi, core=console.core)
    print(f"Baixando núcleo {console.core} ({abi})…", file=sys.stderr)
    payload = _download(url)
    with zipfile.ZipFile(io.BytesIO(payload)) as zf:
        so_files = [n for n in zf.namelist() if n.endswith(".so")]
        if not so_files:
            raise RuntimeError(f"O pacote {url} não contém nenhuma biblioteca .so")
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp = target.with_suffix(".part")
        with zf.open(so_files[0]) as src, tmp.open("wb") as dst:
            shutil.copyfileobj(src, dst)
        tmp.replace(target)
    return target


def desktop_core_path(console: Console) -> Path:
    """Core built for this computer, used to capture a frame for the icon."""
    system = platform.system()
    arch = {"arm64": "arm64", "aarch64": "arm64", "x86_64": "x86_64", "AMD64": "x86_64"}.get(platform.machine())
    if system == "Darwin" and arch:
        url = DESKTOP_URL.format(platform=f"apple/osx/{arch}", core=console.core, ext="dylib")
    elif system == "Linux" and arch == "x86_64":
        url = DESKTOP_URL.format(platform="linux/x86_64", core=console.core, ext="so")
    else:
        raise RuntimeError(f"captura de tela não suportada em {system}/{platform.machine()}")

    name = url.rsplit("/", 1)[1][:-len(".zip")]
    target = home() / "cores" / "desktop" / name
    if target.exists():
        return target
    print(f"Baixando núcleo {console.core} para este computador…", file=sys.stderr)
    with zipfile.ZipFile(io.BytesIO(_download(url))) as zf:
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp = target.with_suffix(".part")
        with zf.open(name) as src, tmp.open("wb") as dst:
            shutil.copyfileobj(src, dst)
        tmp.replace(target)
    return target


def _download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "rom2apk"})
    with urllib.request.urlopen(request, timeout=120) as response:
        total = int(response.headers.get("Content-Length") or 0)
        chunks = []
        received = 0
        while True:
            chunk = response.read(1 << 16)
            if not chunk:
                break
            chunks.append(chunk)
            received += len(chunk)
            if total and sys.stderr.isatty():
                print(f"\r  {received * 100 // total:3d}% de {total / 1e6:.1f} MB", end="", file=sys.stderr)
        if total and sys.stderr.isatty():
            print(file=sys.stderr)
    return b"".join(chunks)
