"""Minimal headless libretro frontend: runs a game for a few seconds and saves its best-looking frame.

Runs in a separate process (`python -m rom2apk.snapshot CORE GAME OUT.png`) because a misbehaving
core can crash the interpreter.
"""

from __future__ import annotations

import ctypes
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import List, Optional, Tuple

from . import png

# libretro.h constants
ENV_GET_CAN_DUPE = 3
ENV_GET_SYSTEM_DIRECTORY = 9
ENV_SET_PIXEL_FORMAT = 10
ENV_GET_SAVE_DIRECTORY = 31
ENV_EXPERIMENTAL = 0x10000
DEVICE_JOYPAD = 1
JOYPAD_START = 3
PIXEL_0RGB1555, PIXEL_XRGB8888, PIXEL_RGB565 = 0, 1, 2

FPS_FALLBACK = 60.0
ATTRACT_SECONDS = 14   # title screens and attract mode usually show up within this window
AFTER_START_SECONDS = 8
SAMPLE_EVERY = 8       # frames


class GameInfo(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char_p), ("data", ctypes.c_void_p), ("size", ctypes.c_size_t), ("meta", ctypes.c_char_p)]


class SystemInfo(ctypes.Structure):
    _fields_ = [
        ("library_name", ctypes.c_char_p),
        ("library_version", ctypes.c_char_p),
        ("valid_extensions", ctypes.c_char_p),
        ("need_fullpath", ctypes.c_bool),
        ("block_extract", ctypes.c_bool),
    ]


class Geometry(ctypes.Structure):
    _fields_ = [
        ("base_width", ctypes.c_uint),
        ("base_height", ctypes.c_uint),
        ("max_width", ctypes.c_uint),
        ("max_height", ctypes.c_uint),
        ("aspect_ratio", ctypes.c_float),
    ]


class Timing(ctypes.Structure):
    _fields_ = [("fps", ctypes.c_double), ("sample_rate", ctypes.c_double)]


class AvInfo(ctypes.Structure):
    _fields_ = [("geometry", Geometry), ("timing", Timing)]


EnvironmentCb = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
VideoCb = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_size_t)
AudioCb = ctypes.CFUNCTYPE(None, ctypes.c_int16, ctypes.c_int16)
AudioBatchCb = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t)
InputPollCb = ctypes.CFUNCTYPE(None)
InputStateCb = ctypes.CFUNCTYPE(ctypes.c_int16, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint)


class Frame:
    def __init__(self, raw: bytes, width: int, height: int, pitch: int, pixel_format: int):
        self.raw, self.width, self.height, self.pitch, self.format = raw, width, height, pitch, pixel_format

    @property
    def bytes_per_pixel(self) -> int:
        return 4 if self.format == PIXEL_XRGB8888 else 2

    def keys(self, step: int = 2) -> List[int]:
        """Raw pixel values on a sparse grid, enough to compare frames cheaply."""
        view = memoryview(self.raw).cast("I" if self.bytes_per_pixel == 4 else "H")
        per_row = self.pitch // self.bytes_per_pixel
        values: List[int] = []
        for y in range(0, self.height, step):
            start = y * per_row
            values.extend(view[start:start + self.width:step])
        return values

    def rgb(self) -> List[List[Tuple[int, int, int]]]:
        view = memoryview(self.raw).cast("I" if self.bytes_per_pixel == 4 else "H")
        per_row = self.pitch // self.bytes_per_pixel
        convert = {PIXEL_XRGB8888: _from_xrgb8888, PIXEL_RGB565: _from_rgb565, PIXEL_0RGB1555: _from_0rgb1555}[self.format]
        return [[convert(v) for v in view[y * per_row:y * per_row + self.width]] for y in range(self.height)]


def _from_xrgb8888(v: int) -> Tuple[int, int, int]:
    return (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF


def _from_rgb565(v: int) -> Tuple[int, int, int]:
    r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _from_0rgb1555(v: int) -> Tuple[int, int, int]:
    r, g, b = (v >> 10) & 0x1F, (v >> 5) & 0x1F, v & 0x1F
    return (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)


def score(frame: Frame) -> float:
    """Favors colorful, detailed frames over blank, fading or single-color ones."""
    keys = frame.keys()
    if not keys:
        return 0.0
    counts = Counter(keys)
    dominant = counts.most_common(1)[0][1] / len(keys)
    return min(len(counts), 256) * (1.0 - dominant)


def capture(core_path: Path, game_path: Path) -> Tuple[Frame, float]:
    """Returns the best frame and the display aspect ratio reported by the core."""
    core = ctypes.CDLL(str(core_path))
    work_dir = tempfile.mkdtemp(prefix="rom2apk-snapshot-")
    work_dir_c = ctypes.create_string_buffer(work_dir.encode())
    state = {"format": PIXEL_0RGB1555, "frame_index": 0, "sample": False, "frame": None, "start": False}

    def environment(cmd: int, data: int) -> bool:
        cmd &= ~ENV_EXPERIMENTAL
        if cmd == ENV_GET_CAN_DUPE:
            ctypes.cast(data, ctypes.POINTER(ctypes.c_bool))[0] = True
            return True
        if cmd in (ENV_GET_SYSTEM_DIRECTORY, ENV_GET_SAVE_DIRECTORY):
            ctypes.cast(data, ctypes.POINTER(ctypes.c_void_p))[0] = ctypes.addressof(work_dir_c)
            return True
        if cmd == ENV_SET_PIXEL_FORMAT:
            state["format"] = ctypes.cast(data, ctypes.POINTER(ctypes.c_int))[0]
            return True
        return False

    def video(data: int, width: int, height: int, pitch: int) -> None:
        if data and state["sample"]:
            raw = ctypes.string_at(data, pitch * height)
            state["frame"] = Frame(raw, width, height, pitch, state["format"])

    def input_state(port: int, device: int, index: int, button: int) -> int:
        return int(state["start"] and port == 0 and device == DEVICE_JOYPAD and button == JOYPAD_START)

    callbacks = (
        EnvironmentCb(environment),
        VideoCb(video),
        AudioCb(lambda left, right: None),
        AudioBatchCb(lambda data, frames: frames),
        InputPollCb(lambda: None),
        InputStateCb(input_state),
    )
    core.retro_set_environment(callbacks[0])
    core.retro_init()
    core.retro_set_video_refresh(callbacks[1])
    core.retro_set_audio_sample(callbacks[2])
    core.retro_set_audio_sample_batch(callbacks[3])
    core.retro_set_input_poll(callbacks[4])
    core.retro_set_input_state(callbacks[5])

    system = SystemInfo()
    core.retro_get_system_info(ctypes.byref(system))
    content = b"" if system.need_fullpath else game_path.read_bytes()
    buffer = ctypes.create_string_buffer(content, len(content)) if content else None
    info = GameInfo(str(game_path).encode(), ctypes.cast(buffer, ctypes.c_void_p) if buffer else None, len(content), None)
    core.retro_load_game.restype = ctypes.c_bool
    if not core.retro_load_game(ctypes.byref(info)):
        raise RuntimeError("o núcleo não conseguiu abrir o jogo")

    av = AvInfo()
    core.retro_get_system_av_info(ctypes.byref(av))
    fps = av.timing.fps or FPS_FALLBACK

    best: Optional[Frame] = None
    best_score = -1.0

    def run(seconds: float, weight: float, press_start: bool) -> None:
        nonlocal best, best_score
        for i in range(int(seconds * fps)):
            state["start"] = press_start and i < 6
            state["sample"] = i % SAMPLE_EVERY == 0
            core.retro_run()
            frame = state["frame"]
            if state["sample"] and frame is not None:
                value = score(frame) * weight
                if value > best_score:
                    best, best_score = frame, value
                state["frame"] = None

    run(ATTRACT_SECONDS, 1.0, press_start=False)
    run(AFTER_START_SECONDS, 0.85, press_start=True)
    if best is None or best_score <= 0:
        raise RuntimeError("nenhum quadro útil foi gerado")

    geometry = av.geometry
    aspect = geometry.aspect_ratio if geometry.aspect_ratio > 0 else best.width / best.height
    return best, aspect


def main(argv: List[str]) -> int:
    core_path, game_path, out_path = map(Path, argv[1:4])
    frame, aspect = capture(core_path, game_path)
    from .icon import compose
    png.write(out_path, compose(frame.rgb(), aspect))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
