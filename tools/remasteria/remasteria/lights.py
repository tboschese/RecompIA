"""A game's dynamic lights, named by tile, and the tools to find and check them.

The engine lights a scene from what glows in it (remaster.c). It can find glowing things by
look, but a look that fits every game also takes an orange awning or a white letter for a flame.
So each game keeps its own list, games/<jogo>/lights.txt: one line per tile, with the same key
the HD packs use (the tile's graphics and palette, engine/src/hdpack.c).

    key               kind    luma  r   g   b   radius
    6f0e3c55a1b2d4e8  flame   180   -   -   -   -

kind is flame (flickers), glow, window (daylight falls into the room below it) or none (never a
light). luma: the tile's pixels glow from that brightness up. r g b radius: "-" takes them from
what is drawn. ? is a tile not reviewed yet; the engine ignores it.

The work for a new game:

1. survey: the engine plays the game by itself, saving a picture and a state every few seconds,
   and every tile its detection takes for a light (the app does the same while it is played,
   with light_capture on).
2. review: groups those tiles by light, adds them to lights.txt as ? and draws a page with each.
3. decide: 12=flame 13=none 14-20=glow sets the kind of whole groups at once.
4. test: draws every saved scene with the list and compares with the run before.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path

import numpy as np

from . import cores, macapp
from snesre import tracer

KINDS = ("flame", "glow", "window", "none")
SUGGESTED = {"f": "flame", "g": "glow", "w": "window"}
FRAME = {"flame": (255, 140, 0), "glow": (0, 220, 255), "window": (255, 255, 255), "none": (255, 0, 255), "?": (255, 0, 255)}
GROUP = re.compile(r"^## (\d+) (-?\d+),(-?\d+)-(-?\d+),(-?\d+) sugest[aã]o (\w+) (.+)$")


def _app(game: Path) -> dict:
    return json.loads((game / "game.json").read_text())["app"]


def lights_file(game: Path) -> Path:
    return game / _app(game).get("lights", "lights.txt")


def capture_dirs(game: Path) -> list[Path]:
    app = _app(game)
    return [macapp.build_dir(app) / "luzes",
            Path.home() / "Library" / "Application Support" / "RemasterIA" / app["title"] / "luzes"]


# ---- images, with no dependency beyond numpy ----

def read_ppm(path: Path) -> np.ndarray:
    data = path.read_bytes()
    parts, at = [], 0
    while len(parts) < 4:
        while data[at:at + 1].isspace():
            at += 1
        start = at
        while not data[at:at + 1].isspace():
            at += 1
        parts.append(data[start:at])
    width, height = int(parts[1]), int(parts[2])
    return np.frombuffer(data, np.uint8, width * height * 3, at + 1).reshape(height, width, 3)


def write_png(path: Path, image: np.ndarray) -> None:
    image = np.ascontiguousarray(image.astype(np.uint8))
    height, width = image.shape[:2]
    raw = b"".join(b"\x00" + image[y].tobytes() for y in range(height))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


DIGITS = ["111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
          "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"]


def _number(image: np.ndarray, text: str, x: int, y: int, scale: int = 3) -> None:
    for i, ch in enumerate(text):
        glyph = DIGITS[int(ch)]
        for gy in range(5):
            for gx in range(3):
                if glyph[gy * 3 + gx] == "1":
                    y0, x0 = y + gy * scale, x + (i * 4 + gx) * scale
                    image[y0:y0 + scale, x0:x0 + scale] = (255, 255, 0)


def _box(image: np.ndarray, x0: int, y0: int, x1: int, y1: int, color, width: int = 2) -> None:
    h, w = image.shape[:2]
    x0, y0, x1, y1 = max(0, x0), max(0, y0), min(w - 1, x1), min(h - 1, y1)
    image[y0:y0 + width, x0:x1 + 1] = color
    image[max(y0, y1 - width + 1):y1 + 1, x0:x1 + 1] = color
    image[y0:y1 + 1, x0:x0 + width] = color
    image[y0:y1 + 1, max(x0, x1 - width + 1):x1 + 1] = color


# ---- lights.txt ----

def read_lights(path: Path) -> tuple[list[str], dict[str, int]]:
    """The file's lines, and the line of each tile key."""
    lines = path.read_text().splitlines() if path.exists() else []
    where = {}
    for i, line in enumerate(lines):
        fields = line.split()
        if fields and not line.startswith("#") and re.fullmatch(r"[0-9a-f]{16}", fields[0]):
            where[fields[0]] = i
    return lines, where


HEADER = """# RemasterIA: the dynamic lights of this game, by tile (tools/remasteria/remasteria/lights.py).
# key               kind    luma  r   g   b   radius
# kind: flame, glow, window, none; ? = not reviewed yet (ignored). "-": taken from what is drawn.
"""


def candidates(dirs: list[Path]) -> list[dict]:
    """Detection's lights from capture folders: one entry per light, with its tiles."""
    groups: dict[tuple, dict] = {}
    for folder in dirs:
        listing = folder / "candidates.tsv"
        if not listing.exists():
            continue
        for line in listing.read_text().splitlines():
            fields = line.split("\t")
            if len(fields) != 12:
                continue
            key, kind, luma, r, g, b, radius, picture, x0, y0, x1, y1 = fields
            box = tuple(int(v) for v in (x0, y0, x1, y1))
            group = groups.setdefault((str(folder / picture), box), {
                "picture": folder / picture, "box": box, "kind": SUGGESTED.get(kind, "glow"), "tiles": {},
                "light": (int(r), int(g), int(b), int(radius))})
            group["tiles"].setdefault(key, int(luma))
    return list(groups.values())


def review(game: Path, dirs: list[Path] | None = None) -> tuple[Path, int, int]:
    """Adds the tiles not in lights.txt as ?, grouped by light, and draws the review page.
    Returns the page, the groups added and the groups still to decide."""
    path = lights_file(game)
    lines, where = read_lights(path)
    if not lines:
        lines = HEADER.rstrip("\n").splitlines()
    number = max([int(m.group(1)) for m in map(GROUP.match, lines) if m] + [0])
    fresh: list[dict] = []
    for group in candidates(dirs or capture_dirs(game)):
        # The color the detection saw, until someone sets it. Not its reach: a tile is often one
        # piece of the light, and the reach follows the piece.
        r, g, b, _ = group["light"]
        for key in group["tiles"].keys() & where.keys():
            fields = lines[where[key]].split()
            if fields[3:6] == ["-"] * 3:
                lines[where[key]] = f"{fields[0]}  {fields[1]:<7} {fields[2]:<4}  {r:<3} {g:<3} {b:<3} {fields[6]}"
        new = {key: luma for key, luma in group["tiles"].items() if key not in where}
        if not new:
            continue
        for key in new:
            where[key] = -1
        # An animated light (a torch, lava) shows other tiles in each drawing, found in many
        # scenes: one group where the same light sits in the same place.
        same = next((f for f in fresh if f["kind"] == group["kind"] and _same_light(f, group)), None)
        if same:
            same["tiles"].update(new)
        else:
            fresh.append(dict(group, tiles=new))
    for group in fresh:
        number += 1
        r, g, b, _ = group["light"]
        x0, y0, x1, y1 = group["box"]
        lines.append("")
        lines.append(f"## {number} {x0},{y0}-{x1},{y1} sugestão {group['kind']} {group['picture']}")
        for key, luma in sorted(group["tiles"].items()):
            lines.append(f"{key}  ?       {luma:<4}  {r:<3} {g:<3} {b:<3} -")
    added = len(fresh)
    path.write_text("\n".join(lines) + "\n")
    page, pending = draw_review(game)
    return page, added, pending


def _same_light(a: dict, b: dict) -> bool:
    """Two finds of one light: about the same color, at about the same height on the screen
    (a figure walking in front cuts a band of lava or a row of lamps somewhere else each time)."""
    if max(abs(x - y) for x, y in zip(a["light"][:3], b["light"][:3])) > 32:
        return False
    _, ay0, _, ay1 = a["box"]
    _, by0, _, by1 = b["box"]
    overlap = min(ay1, by1) - max(ay0, by0) + 1
    return overlap * 2 >= min(ay1 - ay0 + 1, by1 - by0 + 1)


def _groups(lines: list[str]) -> list[dict]:
    groups, current = [], None
    for line in lines:
        match = GROUP.match(line)
        if match:
            n, x0, y0, x1, y1, kind, picture = match.groups()
            current = {"n": int(n), "picture": Path(picture), "box": (int(x0), int(y0), int(x1), int(y1)),
                       "suggested": kind, "kinds": set(), "tiles": 0}
            groups.append(current)
        elif current is not None and line and not line.startswith("#"):
            current["kinds"].add(line.split()[1])
            current["tiles"] += 1
    return groups


def _cell(group: dict, size: tuple[int, int] = (200, 160)) -> np.ndarray:
    cell = np.zeros((size[1], size[0], 3), np.uint8)
    picture = group["picture"]
    if picture.exists():
        scene = read_ppm(picture)
        x0, y0, x1, y1 = group["box"]
        margin = 20
        cx0, cy0 = max(0, x0 - margin), max(0, y0 - margin)
        cx1, cy1 = min(scene.shape[1], x1 + margin + 1), min(scene.shape[0], y1 + margin + 1)
        crop = scene[cy0:cy1, cx0:cx1]
        scale = max(1, min((size[0] - 8) // crop.shape[1], (size[1] - 24) // crop.shape[0]))
        big = crop.repeat(scale, 0).repeat(scale, 1)
        oy, ox = 22, (size[0] - big.shape[1]) // 2
        cell[oy:oy + big.shape[0], ox:ox + big.shape[1]] = big
        kind = next(iter(group["kinds"])) if len(group["kinds"]) == 1 else "?"
        _box(cell, ox + (x0 - cx0) * scale - 2, oy + (y0 - cy0) * scale - 2,
             ox + (x1 - cx0 + 1) * scale + 1, oy + (y1 - cy0 + 1) * scale + 1,
             FRAME.get(kind if kind != "?" else group["suggested"], (255, 0, 255)))
    _number(cell, str(group["n"]), 4, 4)
    return cell


def draw_review(game: Path) -> tuple[Path, int]:
    """build/<id>/luzes-revisao: index.html, one picture per group and contact sheets of the
    groups still to decide (the border is the suggested kind: orange flame, cyan glow, white
    window)."""
    lines, _ = read_lights(lights_file(game))
    groups = _groups(lines)
    out = macapp.build_dir(_app(game)) / "luzes-revisao"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    rows, pending = [], [g for g in groups if "?" in g["kinds"]]
    for group in groups:
        write_png(out / f"{group['n']}.png", _cell(group))
        decided = "/".join(sorted(group["kinds"]))
        rows.append(f"<figure class='{'pending' if '?' in group['kinds'] else 'done'}'>"
                    f"<img src='{group['n']}.png'><figcaption>#{group['n']} · sugestão <b>{group['suggested']}</b>"
                    f" · agora <b>{decided}</b> · {group['tiles']} tiles</figcaption></figure>")
    per_sheet = 30
    for page in range((len(pending) + per_sheet - 1) // per_sheet):
        chunk = pending[page * per_sheet:(page + 1) * per_sheet]
        cells = [_cell(g) for g in chunk]
        sheet = np.zeros((((len(cells) + 5) // 6) * 164, 6 * 204, 3), np.uint8)
        for i, cell in enumerate(cells):
            y, x = (i // 6) * 164, (i % 6) * 204
            sheet[y:y + 160, x:x + 200] = cell
        write_png(out / f"pendentes-{page + 1}.png", sheet)
    (out / "index.html").write_text(
        "<!doctype html><meta charset='utf-8'><title>Luzes</title><style>"
        "body{background:#111;color:#ddd;font:14px system-ui}figure{display:inline-block;margin:6px}"
        "img{image-rendering:pixelated}.done{opacity:.45}</style>"
        f"<h1>Luzes de {_app(game)['title']}</h1><p>Borda: laranja chama, ciano brilho, branco janela. "
        f"Decida com: <code>remasteria lights decide {game} 12=flame 13=none 14-20=glow</code></p>"
        + "".join(rows))
    return out / "index.html", len(pending)


def decide(game: Path, choices: list[str]) -> int:
    """Sets the kind of whole groups: "12=flame", "14-20=glow", "all=none" (every group not
    decided yet), "30=?" (back to the detection). Returns the tiles changed."""
    path = lights_file(game)
    lines, _ = read_lights(path)
    wanted: dict[int, str] = {}
    rest = None
    for choice in choices:
        target, _, kind = choice.partition("=")
        if kind not in KINDS + ("?",):
            raise RuntimeError(f"tipo desconhecido em {choice!r}: use {', '.join(KINDS)} ou ? (a detecção decide)")
        if target == "all":
            rest = kind
            continue
        first, _, last = target.partition("-")
        for n in range(int(first), int(last or first) + 1):
            wanted[n] = kind
    changed, group = 0, None
    for i, line in enumerate(lines):
        match = GROUP.match(line)
        if match:
            group = int(match.group(1))
            continue
        fields = line.split()
        if group is None or not fields or line.startswith("#"):
            continue
        kind = wanted.get(group, rest if fields[1] == "?" else None)
        if kind and fields[1] != kind:
            fields[1] = kind
            lines[i] = f"{fields[0]}  {kind:<7} {'  '.join(fields[2:])}"
            changed += 1
    path.write_text("\n".join(lines) + "\n")
    draw_review(game)
    return changed


# ---- running the engine ----

def _config(game: Path, folder: Path) -> Path:
    app = _app(game)
    config = folder / "game.cfg"
    config.write_text("\n".join(["remaster=on"] + [f"{k}={v}" for k, v in app.get("options", {}).items()]) + "\n")
    return config


def survey(game: Path, rom: Path, frames: int, every: int, press: bool) -> tuple[Path, Path]:
    """The engine plays the game alone (the intro, or with press the menus, by Start and A), and
    saves a picture and a state every `every` frames, and the lights detection finds."""
    app = _app(game)
    build = macapp.build_dir(app)
    states, found = build / "levantamento", build / "luzes"
    states.mkdir(parents=True, exist_ok=True)
    found.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, REMASTERIA_CAPTURE=os.devnull, REMASTERIA_SURVEY=str(states),
                   REMASTERIA_SURVEY_EVERY=str(every), REMASTERIA_LIGHT_CAPTURE=str(found),
                   REMASTERIA_CONFIG=str(_config(game, Path(tmp))))
        env.pop("REMASTERIA_CAPTURE_FRAME", None)
        env["REMASTERIA_SURVEY_PRESS_A" if press else "REMASTERIA_HEADLESS_IDLE"] = "1"
        rom_file = Path(tmp) / "game.sfc"
        shutil.copy(rom, rom_file)
        subprocess.run([str(tracer.engine_path()), str(cores.core_path(app.get("core", "snes9x"))), str(rom_file),
                        tmp, str(frames)], env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return states, found


def scan(game: Path, rom: Path, frames: int = 45, workers: int = 8, report=print, use_list: bool = True,
         folder: str = "luzes-varredura") -> list[Path]:
    """The detection on every scene of the sweep (remasteria sweep), each played on for `frames`
    with no input, so a flame is seen in more than one drawing. Tiles the list already names are
    not found again (unless not use_list). Returns the folders of what was found, for review."""
    from concurrent.futures import ThreadPoolExecutor
    from . import sweep

    app = _app(game)
    cells_dir = sweep.sweep_dir(game) / "cells"
    cells = sorted(p.name[:-len(".state.z")] for p in cells_dir.glob("*.state.z"))
    if not cells:
        raise RuntimeError(f"nenhuma cena varrida: rode antes remasteria sweep {game} <rom>")
    out = macapp.build_dir(app) / folder
    engine, core = tracer.engine_path(), cores.core_path(app.get("core", "snes9x"))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        config, rom_file = _config(game, tmp), tmp / "game.sfc"
        shutil.copy(rom, rom_file)

        def one(cell: str) -> None:
            found = out / cell
            if (found / "feito").exists():
                return
            work = tmp / cell
            (work / "data").mkdir(parents=True)
            env = {k: v for k, v in os.environ.items() if not k.startswith("REMASTERIA")}
            env.update(REMASTERIA_CONFIG=str(config), REMASTERIA_HEADLESS_FROM=str(sweep.state_file(cells_dir, cell, work)),
                       REMASTERIA_LIGHT_CAPTURE=str(found), REMASTERIA_LIGHT_DETECT="on")
            if use_list and lights_file(game).exists():
                env["REMASTERIA_LIGHTS"] = str(lights_file(game))
            try:
                subprocess.run([str(engine), str(core), str(rom_file), str(work / "data"), str(frames)], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
                found.mkdir(parents=True, exist_ok=True)
                (found / "feito").touch()
            except subprocess.TimeoutExpired:
                pass
            shutil.rmtree(work, ignore_errors=True)

        with ThreadPoolExecutor(workers) as pool:
            for i, _ in enumerate(pool.map(one, cells)):
                if (i + 1) % 200 == 0:
                    report(f"{i + 1}/{len(cells)} cenas")
    return [d for d in sorted(out.iterdir()) if (d / "candidates.tsv").exists()]


def score(game: Path, rom: Path, workers: int = 8, report=print) -> dict:
    """How the detection alone does on the swept scenes, against what the review decided: the
    lights it finds that were judged lights, and those judged not to be. The engine is part of
    the folder's name, so a change to the detection is a new score."""
    lines, where = read_lights(lights_file(game))
    decided = {key: lines[i].split()[1] for key, i in where.items()}
    engine = tracer.engine_path().name
    found = scan(game, rom, workers=workers, report=report, use_list=False, folder=f"luzes-nota/{engine}")
    tiles: dict[str, str] = {}
    for folder in found:
        for line in (folder / "candidates.tsv").read_text().splitlines():
            tiles.setdefault(line.split("\t")[0], folder.name)
    counts = {"luz": 0, "não": 0, "sem revisão": 0}
    wrong: dict[str, int] = {}
    for key, cell in tiles.items():
        kind = decided.get(key, "?")
        group = "luz" if kind in ("flame", "glow", "window") else "não" if kind == "none" else "sem revisão"
        counts[group] += 1
        if group == "não":
            wrong[cell] = wrong.get(cell, 0) + 1
    lit = {k for k, v in decided.items() if v in ("flame", "glow", "window")}
    return {"engine": engine, "counts": counts, "lights_found": len(lit & tiles.keys()), "lights": len(lit),
            "wrong_scenes": sorted(wrong.items(), key=lambda kv: -kv[1])[:10]}


def test(game: Path, rom: Path, states: Path | None, frame: int, detect: bool) -> dict:
    """Draws every saved scene with the game's lights; compares with the run before. The result:
    the run's folder, the scenes that changed, the slowest remaster time."""
    app = _app(game)
    build = macapp.build_dir(app)
    states = states or build / "levantamento"
    runs = build / "luzes-teste"
    runs.mkdir(parents=True, exist_ok=True)
    before = sorted(p for p in runs.iterdir() if p.is_dir())
    run = runs / time.strftime("%Y%m%d-%H%M%S")
    run.mkdir()
    engine, core = tracer.engine_path(), cores.core_path(app.get("core", "snes9x"))
    timings = {}
    with tempfile.TemporaryDirectory() as tmp:
        config, rom_file = _config(game, Path(tmp)), Path(tmp) / "game.sfc"
        shutil.copy(rom, rom_file)
        for state in sorted(states.glob("*.state")):
            data = Path(tmp) / state.stem
            data.mkdir()
            env = dict(os.environ, REMASTERIA_CONFIG=str(config), REMASTERIA_CAPTURE_STATE=str(state),
                       REMASTERIA_CAPTURE=str(run / f"{state.stem}.ppm"), REMASTERIA_CAPTURE_SRC=str(run / f"{state.stem}-src.ppm"),
                       REMASTERIA_CAPTURE_FRAME=str(frame), REMASTERIA_REMASTER="1", REMASTERIA_STATS="1",
                       REMASTERIA_LIGHT_DETECT="on" if detect else "off")
            if lights_file(game).exists():
                env["REMASTERIA_LIGHTS"] = str(lights_file(game))
            out = subprocess.run([str(engine), str(core), str(rom_file), str(data)], env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300).stdout
            ms = [float(v) for v in re.findall(r"remaster ([0-9.]+) ms", out)]
            if ms:
                timings[state.stem] = max(ms)
    (run / "tempos.json").write_text(json.dumps(timings, indent=1))
    changed = []
    if before:
        last = before[-1]
        for picture in sorted(run.glob("*.ppm")):
            if picture.stem.endswith("-src"):
                continue
            old = last / picture.name
            if not old.exists() or old.read_bytes() != picture.read_bytes():
                changed.append(picture.stem)
        _changed_sheet(run, last, changed)
    return {"run": run, "changed": changed, "slowest": max(timings.items(), key=lambda kv: kv[1]) if timings else None}


def _crop(picture: Path) -> np.ndarray:
    image = read_ppm(picture)
    h, w = image.shape[:2]
    side = min(w // 256, h // 224) or 1
    cw, ch = 256 * side, 224 * side
    image = image[(h - ch) // 2:(h - ch) // 2 + ch, (w - cw) // 2:(w - cw) // 2 + cw]
    return image[::side * 2, ::side * 2]


def _changed_sheet(run: Path, last: Path, changed: list[str]) -> None:
    """mudou-N.png: each changed scene, the run before on the left and this one on the right."""
    for page in range((len(changed) + 7) // 8):
        names = changed[page * 8:(page + 1) * 8]
        sheet = np.zeros((len(names) * 116, 2 * 132, 3), np.uint8)
        for i, name in enumerate(names):
            for j, folder in enumerate((last, run)):
                picture = folder / f"{name}.ppm"
                if picture.exists():
                    image = _crop(picture)[:112, :128]
                    sheet[i * 116:i * 116 + image.shape[0], j * 132:j * 132 + image.shape[1]] = image
        write_png(run / f"mudou-{page + 1}.png", sheet)
