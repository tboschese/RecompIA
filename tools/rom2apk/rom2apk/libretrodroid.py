"""Fetches LibretroDroid at a pinned commit and applies rom2apk's patch (custom shader chains)."""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

from .paths import TEMPLATE_DIR, home

REPOSITORY = "https://github.com/Swordfish90/LibretroDroid.git"
COMMIT = "8835c3098514390a271e36983957f7bb5f40abf1"  # 0.14.0
PATCH = TEMPLATE_DIR / "libretrodroid.patch"


class LibretroDroidError(Exception):
    pass


def _patch_id() -> str:
    return hashlib.sha1(PATCH.read_bytes()).hexdigest()[:10]


def prepared_checkout() -> Path:
    """Returns the patched `libretrodroid` module directory, cloning it on first use."""
    target = home() / "libretrodroid" / f"{COMMIT[:10]}-{_patch_id()}"
    module = target / "libretrodroid"
    if module.is_dir():
        return module

    print("Baixando e preparando a LibretroDroid (só na primeira vez)…", file=sys.stderr)
    staging = target.with_name(target.name + ".tmp")
    shutil.rmtree(staging, ignore_errors=True)
    staging.parent.mkdir(parents=True, exist_ok=True)
    try:
        _git("clone", "--quiet", REPOSITORY, str(staging))
        _git("checkout", "--quiet", COMMIT, cwd=staging)
        _git("submodule", "update", "--init", "--depth", "1", "--quiet", cwd=staging)
        _git("apply", str(PATCH), cwd=staging)
    except LibretroDroidError:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    staging.rename(target)
    return module


def install(workspace: Path) -> None:
    module = prepared_checkout()
    destination = workspace / "libretrodroid"
    marker = destination / ".rom2apk-source"
    if marker.exists() and marker.read_text() == str(module):
        return
    shutil.rmtree(destination, ignore_errors=True)
    shutil.copytree(module, destination, ignore=shutil.ignore_patterns("build", ".cxx"))
    marker.write_text(str(module))


def _git(*args: str, cwd: Path = None) -> None:
    git = shutil.which("git")
    if not git:
        raise LibretroDroidError("git não encontrado. Instale com: xcode-select --install")
    result = subprocess.run([git, *args], cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        raise LibretroDroidError(f"git {' '.join(args)} falhou:\n{result.stderr.strip()}")
