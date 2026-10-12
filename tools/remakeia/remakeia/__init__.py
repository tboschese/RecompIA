import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2]
for _tool in ("snesre", "rom2apk"):
    if str(TOOLS / _tool) not in sys.path:
        sys.path.insert(0, str(TOOLS / _tool))
