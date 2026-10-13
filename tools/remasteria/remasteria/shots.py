"""Before and after: the remaster of the swept scenes, side by side, and its time.

    remasteria shots games/<jogo> <rom> [--count 48]

Picks scenes from the sweep (sweep.py) spread over what it found, plays each a moment with no
input and saves the console picture and the remaster of the last frame (engine REMASTERIA_SHOT),
at the scale the app draws on the screen, so the time is the real one. Writes
build/<id>/remaster-teste/<run>/: each scene, pares-N.png (before | after, half size) and
tempos.json.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

from . import cores, macapp
from .lights import _app, _config, lights_file, read_ppm, write_png
from . import sweep
from snesre import tracer


def pick(game: Path, count: int) -> list[str]:
    """Scenes that found the most scenery, one per stretch of the archive, so the picks cover
    the whole sweep and not one busy room."""
    archive = json.loads((sweep.sweep_dir(game) / "archive.json").read_text())
    cells = [c for c in archive["cells"] if (sweep.sweep_dir(game) / "cells" / f"{c['name']}.state.z").exists()]
    if len(cells) <= count:
        return [c["name"] for c in cells]
    chosen = []
    for i in range(count):
        part = cells[i * len(cells) // count:(i + 1) * len(cells) // count]
        best = max(part, key=lambda c: (c.get("new_scenery", 0), c.get("new", 0)))
        chosen.append(best["name"])
    return chosen


def shots(game: Path, rom: Path, cells: list[str], scale: int = 8, frames: int = 40, workers: int = 4,
          lights: bool = True, engine: Path | None = None, debug: bool = False, press: str | None = None) -> dict:
    app = _app(game)
    runs = macapp.build_dir(app) / "remaster-teste"
    run = runs / time.strftime("%Y%m%d-%H%M%S")
    run.mkdir(parents=True)
    engine, core = engine or tracer.engine_path(), cores.core_path(app.get("core", "snes9x"))
    cells_dir = sweep.sweep_dir(game) / "cells"
    timings: dict[str, list[float]] = {}
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        config, rom_file = _config(game, tmp), tmp / "game.sfc"
        shutil.copy(rom, rom_file)

        def one(cell: str) -> None:
            work = tmp / cell
            (work / "data").mkdir(parents=True)
            env = {k: v for k, v in os.environ.items() if not k.startswith("REMASTERIA")}
            env.update(REMASTERIA_CONFIG=str(config), REMASTERIA_HEADLESS_FROM=str(sweep.state_file(cells_dir, cell, work)),
                       REMASTERIA_SHOT=str(run / f"{cell}.ppm"), REMASTERIA_SHOT_SRC=str(run / f"{cell}-src.ppm"),
                       REMASTERIA_SHOT_SCALE=str(scale), REMASTERIA_SHOT_LAYERS=str(run / f"{cell}-layers.pgm"))
            if lights and lights_file(game).exists():
                env["REMASTERIA_LIGHTS"] = str(lights_file(game))
            if debug:
                env["REMASTERIA_LIGHT_DEBUG"] = "1"
            if press:
                env["REMASTERIA_HEADLESS_PRESS"] = press
            try:
                out = subprocess.run([str(engine), str(core), str(rom_file), str(work / "data"), str(frames)], env=env,
                                     stdout=subprocess.PIPE, stderr=open(run / f"{cell}.log", "w") if debug else subprocess.DEVNULL,
                                     text=True, timeout=120).stdout
                found = re.search(r"remaster ([0-9.]+) ms \(máximo ([0-9.]+) ms\)", out)
                if found:
                    timings[cell] = [float(found.group(1)), float(found.group(2))]
            except subprocess.TimeoutExpired:
                pass
            shutil.rmtree(work, ignore_errors=True)

        with ThreadPoolExecutor(workers) as pool:
            list(pool.map(one, cells))
    (run / "tempos.json").write_text(json.dumps(timings, indent=1))
    _pairs(run, [c for c in cells if (run / f"{c}.ppm").exists()], scale)
    worst = max(timings.items(), key=lambda kv: kv[1][1]) if timings else None
    mean = sum(t[0] for t in timings.values()) / len(timings) if timings else 0.0
    return {"run": run, "scenes": len(timings), "mean": mean, "worst": worst}


def _pairs(run: Path, cells: list[str], scale: int, per_page: int = 6) -> None:
    """pares-N.png: the console picture (each pixel `half` wide) and the remaster at half size,
    so the remaster's own steps still show."""
    half = scale // 2
    for page in range((len(cells) + per_page - 1) // per_page):
        rows = []
        for cell in cells[page * per_page:(page + 1) * per_page]:
            after = read_ppm(run / f"{cell}.ppm")[::2, ::2]
            before = read_ppm(run / f"{cell}-src.ppm")
            before = before.repeat(half, 0).repeat(half, 1)[:after.shape[0], :after.shape[1]]
            h = min(before.shape[0], after.shape[0])
            row = np.zeros((h + 8, before.shape[1] + after.shape[1] + 8, 3), np.uint8)
            row[:h, :before.shape[1]] = before[:h]
            row[:h, before.shape[1] + 8:] = after[:h]
            rows.append(row)
        width = max(r.shape[1] for r in rows)
        sheet = np.zeros((sum(r.shape[0] for r in rows), width, 3), np.uint8)
        y = 0
        for r in rows:
            sheet[y:y + r.shape[0], :r.shape[1]] = r
            y += r.shape[0]
        write_png(run / f"pares-{page + 1}.png", sheet)
