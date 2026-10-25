"""Build the native macOS workbench for per-title static recompilation projects."""
from __future__ import annotations

import plistlib
import subprocess
import shutil
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2]


def build(dist: Path) -> Path:
    source = TOOLS.parent / "apps" / "macos"
    _run("swift", "build", "--package-path", str(source), "-c", "release")
    binary_dir = Path(_run("swift", "build", "--package-path", str(source), "-c", "release", "--show-bin-path").strip())
    dist.mkdir(parents=True, exist_ok=True)
    destination = dist.resolve() / "RecompIA.app"
    with tempfile.TemporaryDirectory(prefix=".library-", dir=dist.resolve()) as tmp:
        bundle = Path(tmp) / "RecompIA.app"
        contents = bundle / "Contents"
        macos = contents / "MacOS"
        macos.mkdir(parents=True)
        (contents / "Resources").mkdir()
        executable = macos / "RecompIA"
        shutil.copy(binary_dir / "RecompIA", executable)
        with (contents / "Info.plist").open("wb") as handle:
            plistlib.dump({
                "CFBundleName": "RecompIA", "CFBundleDisplayName": "RecompIA · Recomp",
                "CFBundleIdentifier": "com.recompia.app", "CFBundleExecutable": "RecompIA",
                "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.0.0", "CFBundleVersion": "1",
                "LSMinimumSystemVersion": "13.0", "NSHighResolutionCapable": True,
                "LSApplicationCategoryType": "public.app-category.developer-tools",
                "NSSupportsAutomaticTermination": False,
            }, handle)
        _run("codesign", "--force", "--sign", "-", str(executable))
        _run("codesign", "--force", "--sign", "-", str(bundle))
        _run("codesign", "--verify", "--deep", "--strict", str(bundle))
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(str(bundle), destination)
    return destination


def _run(*command: str) -> str:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise RuntimeError(f"Falhou: {' '.join(command)}\n{result.stdout[-5000:]}")
    return result.stdout
