"""Builds a game's HD art packs from the frames the app captures while it is played.

The engine saves the frames that show new tiles, with the identity of each pixel: which tile (its
graphics and palette) and which texel of it (engine/src/hdpack.c). Here each frame is redrawn at 4x
by an AI upscaler, looking at the whole scene, and cut back into tiles at 2x: every 2x2 block the AI
drew collapses into one flat pixel, so diagonals stay staircases instead of turning into curves.

The pixels are then snapped back to the tile's own original colors. The redraw uses the assets the
game already has: same tiles, same sprites, same palette. It does not repaint them and it does not
touch gameplay. A game only gets a pack when its own `art_styles` says so.

The packs are derived from the ROM, so they stay out of Git (build/<game>/hdpack-<estilo>.bin).
"""

from __future__ import annotations

import gzip
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

import numpy as np

from snesre import tracer

SCALE = 2          # pixels of the pack per original pixel: crisp pixel art, no smoothing
AI_SCALE = 4       # what the upscaler draws; collapsed to SCALE afterwards
TILE = 8 * SCALE
ESRGAN_RELEASE = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/"
ESRGAN_ZIPS = {"Darwin": "realesrgan-ncnn-vulkan-20220424-macos.zip",
               "Linux": "realesrgan-ncnn-vulkan-20220424-ubuntu.zip"}
ESRGAN_MODEL = "realesrgan-x4plus-anime"
EDGE_WEIGHT, INSIDE_WEIGHT = 1.0, 4.0


class Frame:
    def __init__(self, path: Path):
        data = gzip.open(path).read()
        if data[:8] != b"RMXF1\0\0\0":
            raise RuntimeError(f"captura inválida: {path}")
        width, height = struct.unpack_from("<II", data, 8)
        offset = 16
        count = width * height
        self.path, self.width, self.height = path, width, height
        self.pixels = np.frombuffer(data, "<u4", count, offset).reshape(height, width)
        offset += count * 4
        self.keys = np.frombuffer(data, "<u8", count, offset).reshape(height, width)
        offset += count * 8
        self.texels = np.frombuffer(data, "u1", count, offset).reshape(height, width)
        offset += count
        (tiles,) = struct.unpack_from("<I", data, offset)
        offset += 4
        self.tiles = {}
        for _ in range(tiles):
            (key,) = struct.unpack_from("<Q", data, offset)
            self.tiles[key] = np.frombuffer(data, "<u4", 64, offset + 8).reshape(8, 8)
            offset += 8 + 256

    def rgb(self) -> np.ndarray:
        p = self.pixels
        return np.stack([(p >> 16) & 255, (p >> 8) & 255, p & 255], axis=-1).astype(np.uint8)


def esrgan() -> Path:
    folder = tracer.home() / "tools" / "realesrgan"
    binary = folder / "realesrgan-ncnn-vulkan"
    if binary.exists():
        return binary
    name = ESRGAN_ZIPS.get(platform.system())
    if not name:
        raise RuntimeError("o Real-ESRGAN não tem versão pronta para este sistema")
    print("Baixando o Real-ESRGAN (IA de upscaling, só na primeira vez)…", file=sys.stderr)
    folder.mkdir(parents=True, exist_ok=True)
    archive = folder.with_suffix(".zip")
    with urllib.request.urlopen(ESRGAN_RELEASE + name, timeout=300) as response:
        archive.write_bytes(response.read())
    with zipfile.ZipFile(archive) as z:
        z.extractall(folder)
    archive.unlink()
    binary.chmod(0o755)
    if platform.system() == "Darwin":
        subprocess.run(["xattr", "-dr", "com.apple.quarantine", str(folder)], check=False)
    return binary


def upscale(frames: list[Frame], cache: Path, upscaler: str) -> None:
    """Redraws each frame at 4x into cache/<frame>.png (kept between runs)."""
    from PIL import Image

    cache.mkdir(parents=True, exist_ok=True)
    todo = [f for f in frames if not (cache / f"{f.path.stem}.png").exists()]
    if not todo:
        return
    if upscaler == "nearest":
        for f in todo:
            Image.fromarray(f.rgb()).resize((f.width * AI_SCALE, f.height * AI_SCALE), Image.NEAREST).save(cache / f"{f.path.stem}.png")
        return
    binary = esrgan()
    print(f"Redesenhando {len(todo)} cenas com IA ({ESRGAN_MODEL})…", file=sys.stderr)
    with tempfile.TemporaryDirectory() as tmp:
        source, target = Path(tmp) / "in", Path(tmp) / "out"
        source.mkdir()
        target.mkdir()
        for f in todo:
            Image.fromarray(f.rgb()).save(source / f"{f.path.stem}.png")
        result = subprocess.run([str(binary), "-i", str(source), "-o", str(target), "-n", ESRGAN_MODEL, "-s", str(AI_SCALE),
                                 "-f", "png", "-m", str(binary.parent / "models")],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        produced = list(target.glob("*.png"))
        if len(produced) != len(todo):
            raise RuntimeError(f"o Real-ESRGAN falhou:\n{result.stdout[-2000:]}")
        for path in produced:
            shutil.move(str(path), cache / path.name)


# The only style: the game's own tiles, snapped to their own colors. Repainting them into another
# game's look changes the assets, so it is not offered.
STYLES = {
    "remaster": {"name": "remaster fiel", "bloom": 0},
}


def read_pack(path: Path) -> dict[int, np.ndarray]:
    if not path.exists():
        return {}
    data = path.read_bytes()
    if data[:8] == b"RMXHD2\0\0":
        scale = struct.unpack_from("<I", data, 8)[0]
        count = struct.unpack_from("<I", data, 80)[0]
        offset = 84
    elif data[:8] == b"RMXHD1\0\0":
        scale, count = struct.unpack_from("<II", data, 8)
        offset = 16
    else:
        raise RuntimeError(f"pacote inválido: {path}")
    if scale != SCALE:
        return {}
    tiles = {}
    for _ in range(count):
        (key,) = struct.unpack_from("<Q", data, offset)
        tiles[key] = np.frombuffer(data, "<u4", TILE * TILE, offset + 8).reshape(TILE, TILE).copy()
        offset += 8 + TILE * TILE * 4
    return tiles


def write_pack(path: Path, tiles: dict[int, np.ndarray], style_id: str, style_name: str, bloom: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix(".part")
    with partial.open("wb") as f:
        f.write(b"RMXHD2\0\0" + struct.pack("<II", SCALE, bloom))
        f.write(style_id.encode()[:23].ljust(24, b"\0"))
        f.write(style_name.encode()[:39].ljust(40, b"\0"))
        f.write(struct.pack("<I", len(tiles)))
        for key in sorted(tiles):
            f.write(struct.pack("<Q", key))
            f.write(tiles[key].astype("<u4").tobytes())
    partial.replace(path)


def snap_to_palette(rgb: np.ndarray, seen: np.ndarray, palette: np.ndarray) -> np.ndarray:
    """Snaps every pixel to the nearest of the tile's original colors. The upscaler draws smooth
    ramps and anti-aliased edges; restricting the result to the console's own flat colors turns
    those back into hard pixels and staircased edges."""
    points = rgb[seen]
    if not len(points):
        return rgb
    nearest = ((points[:, None, :] - palette[None, :, :]) ** 2).sum(2).argmin(1)
    out = rgb.copy()
    out[seen] = palette[nearest]
    return out


def build(captures: list[Path], directory: Path, cache: Path, styles: list[str], upscaler: str = "esrgan") -> list[tuple[str, int, Path]]:
    from PIL import Image

    paths = sorted(p for folder in captures for p in folder.glob("frame-*.rmxf"))
    if not paths:
        raise RuntimeError("nenhuma captura encontrada: jogue com a captura de arte ligada (art_capture=on)")
    frames = [Frame(p) for p in paths]
    upscale(frames, cache, upscaler)

    originals: dict[int, np.ndarray] = {}
    for f in frames:
        originals.update(f.tiles)
    keys = np.array(sorted(originals), dtype=np.uint64)
    ai_tile = 8 * AI_SCALE
    total = np.zeros((len(keys), ai_tile, ai_tile, 3), np.float64)
    weight = np.zeros((len(keys), ai_tile, ai_tile), np.float64)

    for f in frames:
        art = np.asarray(Image.open(cache / f"{f.path.stem}.png").convert("RGB"), np.float64)
        ys, xs = np.nonzero(f.keys)
        if not len(ys):
            continue
        index = np.searchsorted(keys, f.keys[ys, xs])
        texel = f.texels[ys, xs].astype(np.int64)
        tx, ty = (texel & 7) * AI_SCALE, (texel >> 3 & 7) * AI_SCALE
        hflip, vflip = (texel & 0x40) != 0, (texel & 0x80) != 0
        # Inside: the four neighbors are clean art too, so the AI saw no foreign edge there.
        clean = np.pad(f.keys != 0, 1)
        inside = clean[ys, xs + 1] & clean[ys + 2, xs + 1] & clean[ys + 1, xs] & clean[ys + 1, xs + 2]
        w = np.where(inside, INSIDE_WEIGHT, EDGE_WEIGHT)
        for j in range(AI_SCALE):
            for i in range(AI_SCALE):
                sx = np.where(hflip, AI_SCALE - 1 - i, i)
                sy = np.where(vflip, AI_SCALE - 1 - j, j)
                color = art[ys * AI_SCALE + j, xs * AI_SCALE + i]
                np.add.at(total, (index, ty + sy, tx + sx), color * w[:, None])
                np.add.at(weight, (index, ty + sy, tx + sx), w)

    # Collapse the AI image into crisp pixels: each block of AI pixels becomes one flat color.
    block = AI_SCALE // SCALE
    total = total.reshape(len(keys), TILE, block, TILE, block, 3).sum(axis=(2, 4))
    weight = weight.reshape(len(keys), TILE, block, TILE, block).sum(axis=(2, 4))

    unknown = [style for style in styles if style not in STYLES]
    if unknown:
        raise RuntimeError(f"estilo desconhecido: {', '.join(unknown)} (use {', '.join(STYLES)})")

    made = []
    for style_id in styles:
        style = STYLES[style_id]
        print(f"Gerando o estilo {style['name']}…", file=sys.stderr)
        target = directory / f"hdpack-{style_id}.bin"
        tiles = read_pack(target)
        for n, key in enumerate(keys):
            raw = originals[int(key)]
            base = np.stack([(raw >> 16) & 255, (raw >> 8) & 255, raw & 255], axis=-1).astype(np.float64)
            base = base.repeat(SCALE, 0).repeat(SCALE, 1)
            seen = weight[n] > 0
            rgb = np.where(seen[..., None], total[n] / np.maximum(weight[n], 1e-9)[..., None], base)
            original = np.unique(raw[raw != 0xff000000])
            if len(original):
                palette = np.stack([(original >> 16) & 255, (original >> 8) & 255, original & 255], axis=-1).astype(np.float64)
                rgb = snap_to_palette(rgb, seen, palette)
            rgb = np.clip(rgb + 0.5, 0, 255).astype(np.uint32)
            tiles[int(key)] = rgb[..., 0] << 16 | rgb[..., 1] << 8 | rgb[..., 2]
        write_pack(target, tiles, style_id, style["name"], style["bloom"])
        made.append((style["name"], len(tiles), target))
    return made
