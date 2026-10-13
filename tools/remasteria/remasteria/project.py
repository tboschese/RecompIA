"""A new game, from its ROM: the project folder, the sweep, the lights to review.

    remasteria new roms/<jogo>.sfc

makes games/<id>/game.json (the name from the file, No-Intro style, and the ROM's SHA-1), sweeps
the game (sweep.py), runs the light detection on every scene and draws the review page
(lights.py). After the review: remasteria mac games/<id> <rom> builds the app.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

from . import TOOLS

OPTIONS = {
    "bsnes_ppu_show_overscan": "ON",
    "bsnes_video_aspectcorrection": "OFF",
    "bsnes_ppu_no_sprite_limit": "ON",
    "bsnes_mode7_scale": "4x",
    "bsnes_mode7_perspective": "auto (wide)",
    "bsnes_mode7_supersample": "none",
    "bsnes_mode7_mosaic": "1x scale",
    "bsnes_mode7_wsMode": "none",
    "bsnes_mode7_widescreen": "none",
    "bsnes_dsp_cubic": "ON",
}


def create(rom: Path) -> Path:
    """games/<id>/game.json for this ROM; an existing project is kept as it is."""
    name = rom.stem
    title = re.sub(r"\s*[\(\[].*$", "", name).strip() or name
    ident = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")
    game = TOOLS.parent / "games" / ident
    if (game / "game.json").exists():
        return game
    game.mkdir(parents=True, exist_ok=True)
    (game / "game.json").write_text(json.dumps({
        "name": name,
        "sha1": hashlib.sha1(rom.read_bytes()).hexdigest(),
        "app": {"title": title, "id": ident, "version": "0.1.0", "remaster": True, "lights": "lights.txt",
                "icon_seconds": 30, "core": "bsnes-hd", "video": "pixel", "options": OPTIONS},
        "entries": [],
        "labels": {},
    }, indent=1, ensure_ascii=False) + "\n")
    return game
