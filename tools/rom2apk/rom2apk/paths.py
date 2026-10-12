from __future__ import annotations

import os
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TEMPLATE_DIR = PROJECT_ROOT / "template"


def home() -> Path:
    path = Path(os.environ.get("ROM2APK_HOME", Path.home() / ".rom2apk"))
    path.mkdir(parents=True, exist_ok=True)
    return path
