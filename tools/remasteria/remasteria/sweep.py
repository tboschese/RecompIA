"""The sweep: the whole game, found by the engine playing it alone.

A list of lights (or of art to redraw) has to name every tile the game shows, not only the ones
someone happened to play past. So the engine plays by itself, the way a fuzzer does: from a
saved state, random input for a while, keeping a state whenever the screen shows tiles not seen
before (engine/src/explore.c). Those states are the archive. Each round starts runs from the
states that found the most and were tried the least (Go-Explore), so the sweep goes deeper
instead of replaying the title screen. It stops when the rounds stop finding anything, or at
the time given.

Where the random input can't get (a story event, a boss), a state from someone playing is a
seed: the sweep starts from it too, and the archive grows around it.

    build/<id>/varredura/
        cells/<cell>.state.z, <cell>.png   one scene of the game per cell (the state zlib-packed)
        tiles.tsv                          every tile found: key cell x y layer (4 = sprite) and 1 when
                                           its graphics were new (0: known ones in a new palette)
        archive.json                       the cells, where each came from, how much it found
"""

from __future__ import annotations

import json
import math
import os
import random
import shutil
import subprocess
import tempfile
import time
import zlib
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from . import cores, macapp
from .lights import _app, _config, read_ppm, write_png
from snesre import tracer

def sweep_dir(game: Path) -> Path:
    return macapp.build_dir(_app(game)) / "varredura"


def put_state(source: Path, cells_dir: Path, name: str) -> None:
    (cells_dir / f"{name}.state.z").write_bytes(zlib.compress(source.read_bytes(), 6))


def state_file(cells_dir: Path, name: str, into: Path) -> Path | None:
    """The cell's state, unpacked into `into`; None for power-on."""
    packed = cells_dir / f"{name}.state.z"
    if not packed.exists():
        return None
    path = into / f"{name}.state"
    if not path.exists():
        path.write_bytes(zlib.decompress(packed.read_bytes()))
    return path


def _load(out: Path) -> dict:
    path = out / "archive.json"
    if path.exists():
        return json.loads(path.read_text())
    return {"cells": [{"name": "ligar", "parent": None, "new": 0, "chosen": 0, "found": 0, "seed": True}],
            "rounds": 0, "frames": 0}


def _weight(cell: dict) -> float:
    """Go-Explore: favor cells tried few times, that found a lot themselves or through runs
    from them. New scenery counts far more than new sprite frames: a fighter or a hero has
    thousands of frames of animation, and a sweep led by those never leaves the first room."""
    found = cell["new_scenery"] * 8 + cell["new"] if "new_scenery" in cell else cell["new"]
    return (1.0 + math.log1p(found) + 0.5 * math.log1p(cell.get("found_scenery", cell["found"]))) \
        / math.sqrt(1.0 + cell["chosen"])


def _run(engine: Path, core: Path, rom: Path, config: Path, known: Path, state: Path | None,
         frames: int, seed: int, work: Path) -> Path:
    out = work / "out"
    data = work / "data"
    data.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("REMASTERIA")}
    env.update(REMASTERIA_EXPLORE=str(out), REMASTERIA_EXPLORE_KNOWN=str(known),
               REMASTERIA_EXPLORE_SEED=str(seed), REMASTERIA_CONFIG=str(config))
    if state:
        env["REMASTERIA_EXPLORE_FROM"] = str(state)
    # Random input can crash a game; bsnes then never finishes saving a state. What the run kept
    # before that stands.
    try:
        subprocess.run([str(engine), str(core), str(rom), str(data), str(frames)], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60 + frames / 60)
    except subprocess.TimeoutExpired:
        pass
    return out


def sweep(game: Path, rom: Path, minutes: float, workers: int = 8, frames: int = 1800,
          seeds: list[Path] | None = None, report=print) -> dict:
    app = _app(game)
    out = sweep_dir(game)
    cells_dir = out / "cells"
    cells_dir.mkdir(parents=True, exist_ok=True)
    archive = _load(out)
    names = {c["name"] for c in archive["cells"]}
    for seed in seeds or []:
        for state in sorted(seed.glob("*.state") if seed.is_dir() else [seed]):
            name = f"semente-{state.stem}"
            if name not in names:
                put_state(state, cells_dir, name)
                archive["cells"].append({"name": name, "parent": None, "new": 0, "chosen": 0, "found": 0, "seed": True})
                names.add(name)
    known_path, tiles_path = out / "known.txt", out / "tiles.tsv"
    known = set(known_path.read_text().split()) if known_path.exists() else set()
    catalog = [line.split("\t") for line in tiles_path.read_text().splitlines()] if tiles_path.exists() else []
    tiles_total = len(catalog)
    scenery_total = sum(row[4] != "4" and row[5] == "1" for row in catalog)
    del catalog

    engine, core = tracer.engine_path(), cores.core_path(app.get("core", "snes9x"))
    rng = random.Random()
    started, quiet = time.time(), 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        config, rom_file = _config(game, tmp), tmp / "game.sfc"
        shutil.copy(rom, rom_file)
        while time.time() - started < minutes * 60:
            archive["rounds"] += 1
            round_start = time.time()
            known_path.write_text("\n".join(sorted(known)) + "\n")
            cells = archive["cells"]
            # Seeds nobody started from yet go first; then by weight.
            fresh = [c for c in cells if c.get("seed") and c["chosen"] == 0]
            picks = fresh[:workers]
            while len(picks) < workers:
                picks.append(rng.choices(cells, weights=[_weight(c) for c in cells])[0])
            found_round = scenery_round = 0
            with ThreadPoolExecutor(workers) as pool:
                jobs = {}
                for i, cell in enumerate(picks):
                    cell["chosen"] += 1
                    work = tmp / f"r{archive['rounds']}-{i}"
                    work.mkdir()
                    state = state_file(cells_dir, cell["name"], work)
                    jobs[pool.submit(_run, engine, core, rom_file, config, known_path, state, frames,
                                     rng.randrange(1, 1 << 30), work)] = cell
                for job in as_completed(jobs):
                    parent, run = jobs[job], job.result()
                    found, scenery, listed = _merge(run, parent, archive, known, cells_dir, tiles_path)
                    found_round += found
                    scenery_round += scenery
                    tiles_total += listed
                    shutil.rmtree(run.parent, ignore_errors=True)
            archive["frames"] += frames * workers
            scenery_total += scenery_round
            # Done when the scenery stops growing; sprite frames trickle in for ever.
            quiet = quiet + 1 if scenery_round < max(4, scenery_total // 1000) else 0
            (out / "archive.json").write_text(json.dumps(archive, indent=1))
            report(f"rodada {archive['rounds']}: {found_round} desenhos novos ({scenery_round} de cenário), "
                   f"{tiles_total} tiles no catálogo, {scenery_total} desenhos de cenário, {len(archive['cells'])} cenas "
                   f"({time.time() - round_start:.0f} s)")
            if quiet >= 10:
                report("parou: dez rodadas seguidas sem cenário novo")
                break
    known_path.write_text("\n".join(sorted(known)) + "\n")
    return {"dir": out, "tiles": tiles_total, "scenery": scenery_total, "cells": len(archive["cells"]),
            "rounds": archive["rounds"]}


def _merge(run: Path, parent: dict, archive: dict, known: set, cells_dir: Path, tiles_path: Path) -> tuple[int, int]:
    """Adds a run's scenes to the archive, in the order it found them. Another run of the round
    may have found the same tiles first; only what is still new counts."""
    listing = run / "keys.tsv"
    if not listing.exists():
        return 0, 0, 0
    by_scene: dict[str, list[list[str]]] = {}
    for line in listing.read_text().splitlines():
        fields = line.split("\t")
        if len(fields) == 6:
            by_scene.setdefault(fields[1], []).append(fields)
    found, scenery, lines = 0, 0, []
    for scene in sorted(by_scene):
        rows = [r for r in by_scene[scene] if r[0] not in known]
        if not rows or not (run / f"{scene}.state").exists():
            continue
        # New graphics, not known ones in a new palette, measure what a scene found.
        drawn = [r for r in rows if r[5] != "0000000000000000" and r[5] not in known]
        for r in rows:
            known.add(r[0])
        for r in drawn:
            known.add(r[5])
        new_scenery = sum(r[4] != "4" for r in drawn)
        found += len(drawn)
        scenery += new_scenery
        name = f"c{len(archive['cells']):05d}"
        put_state(run / f"{scene}.state", cells_dir, name)
        if (run / f"{scene}.ppm").exists():
            write_png(cells_dir / f"{name}.png", read_ppm(run / f"{scene}.ppm"))
        archive["cells"].append({"name": name, "parent": parent["name"], "new": len(drawn), "new_scenery": new_scenery,
                                 "chosen": 0, "found": 0, "found_scenery": 0})
        drawn_keys = {r[0] for r in drawn}
        lines += [f"{r[0]}\t{name}\t{r[2]}\t{r[3]}\t{r[4]}\t{int(r[0] in drawn_keys)}" for r in rows]
    parent["found"] += found
    parent["found_scenery"] = parent.get("found_scenery", 0) + scenery
    with tiles_path.open("a") as f:
        f.writelines(line + "\n" for line in lines)
    return found, scenery, len(lines)
