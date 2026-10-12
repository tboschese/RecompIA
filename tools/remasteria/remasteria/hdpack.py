"""Builds a game's HD art pack (hdpack.bin) from the frames the app captures while it is played.

The engine saves the frames that show new tiles, with the identity of each pixel: which tile (its
graphics and palette) and which texel of it (engine/src/hdpack.c). Here each frame is redrawn at 4x
by an AI upscaler, looking at the whole scene, and cut back into tiles: every texel of every tile
gets the 4x4 block the AI drew for it, averaged over all the scenes where the tile appeared. Texels
next to other art weigh more than ones at the edge of a sprite, where the AI mixes in what was
behind. Texels never seen clean keep the original pixel, enlarged.

The pack is derived from the ROM, so it stays out of Git (build/<game>/hdpack.bin); the app
generator ships it inside the app.
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

SCALE = 4
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
            Image.fromarray(f.rgb()).resize((f.width * SCALE, f.height * SCALE), Image.NEAREST).save(cache / f"{f.path.stem}.png")
        return
    binary = esrgan()
    print(f"Redesenhando {len(todo)} cenas com IA ({ESRGAN_MODEL})…", file=sys.stderr)
    with tempfile.TemporaryDirectory() as tmp:
        source, target = Path(tmp) / "in", Path(tmp) / "out"
        source.mkdir()
        target.mkdir()
        for f in todo:
            Image.fromarray(f.rgb()).save(source / f"{f.path.stem}.png")
        result = subprocess.run([str(binary), "-i", str(source), "-o", str(target), "-n", ESRGAN_MODEL, "-s", str(SCALE),
                                 "-f", "png", "-m", str(binary.parent / "models")],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        produced = list(target.glob("*.png"))
        if len(produced) != len(todo):
            raise RuntimeError(f"o Real-ESRGAN falhou:\n{result.stdout[-2000:]}")
        for path in produced:
            shutil.move(str(path), cache / path.name)


def read_pack(path: Path) -> dict[int, np.ndarray]:
    if not path.exists():
        return {}
    data = path.read_bytes()
    if data[:8] != b"RMXHD1\0\0":
        raise RuntimeError(f"pacote inválido: {path}")
    scale, count = struct.unpack_from("<II", data, 8)
    if scale != SCALE:
        return {}
    tiles, offset = {}, 16
    for _ in range(count):
        (key,) = struct.unpack_from("<Q", data, offset)
        tiles[key] = np.frombuffer(data, "<u4", TILE * TILE, offset + 8).reshape(TILE, TILE)
        offset += 8 + TILE * TILE * 4
    return tiles


def write_pack(path: Path, tiles: dict[int, np.ndarray]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix(".part")
    with partial.open("wb") as f:
        f.write(b"RMXHD1\0\0" + struct.pack("<II", SCALE, len(tiles)))
        for key in sorted(tiles):
            f.write(struct.pack("<Q", key))
            f.write(tiles[key].astype("<u4").tobytes())
    partial.replace(path)


def build(captures: list[Path], out: Path, cache: Path, upscaler: str = "esrgan") -> int:
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
    total = np.zeros((len(keys), TILE, TILE, 3), np.float64)
    weight = np.zeros((len(keys), TILE, TILE), np.float64)

    for f in frames:
        art = np.asarray(Image.open(cache / f"{f.path.stem}.png").convert("RGB"), np.float64)
        ys, xs = np.nonzero(f.keys)
        if not len(ys):
            continue
        index = np.searchsorted(keys, f.keys[ys, xs])
        texel = f.texels[ys, xs].astype(np.int64)
        tx, ty = (texel & 7) * SCALE, (texel >> 3 & 7) * SCALE
        hflip, vflip = (texel & 0x40) != 0, (texel & 0x80) != 0
        # Inside: the four neighbors are clean art too, so the AI saw no foreign edge there.
        clean = np.pad(f.keys != 0, 1)
        inside = clean[ys, xs + 1] & clean[ys + 2, xs + 1] & clean[ys + 1, xs] & clean[ys + 1, xs + 2]
        w = np.where(inside, INSIDE_WEIGHT, EDGE_WEIGHT)
        for j in range(SCALE):
            for i in range(SCALE):
                sx = np.where(hflip, SCALE - 1 - i, i)
                sy = np.where(vflip, SCALE - 1 - j, j)
                color = art[ys * SCALE + j, xs * SCALE + i]
                np.add.at(total, (index, ty + sy, tx + sx), color * w[:, None])
                np.add.at(weight, (index, ty + sy, tx + sx), w)

    tiles = read_pack(out)
    for n, key in enumerate(keys):
        raw = originals[int(key)]
        base = np.stack([(raw >> 16) & 255, (raw >> 8) & 255, raw & 255], axis=-1).astype(np.float64)
        base = base.repeat(SCALE, 0).repeat(SCALE, 1)
        seen = weight[n] > 0
        rgb = np.where(seen[..., None], total[n] / np.maximum(weight[n], 1e-9)[..., None], base)
        rgb = np.clip(rgb + 0.5, 0, 255).astype(np.uint32)
        tiles[int(key)] = rgb[..., 0] << 16 | rgb[..., 1] << 8 | rgb[..., 2]
    write_pack(out, tiles)
    return len(keys)
