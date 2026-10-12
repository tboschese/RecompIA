"""macOS app icon from a gameplay frame: the rom2apk framing, inside the standard rounded square.

Runs in its own process (`python -m remasteria.icon CORE ROM SECONDS OUT.png`) because the core may
crash. SECONDS picks the moment of the intro to capture (no buttons pressed); 0 lets rom2apk pick
the most colorful frame on its own.
"""

from __future__ import annotations

import ctypes
import sys
import tempfile
from pathlib import Path

from . import TOOLS  # noqa: F401  (puts rom2apk on the path)
from rom2apk import png, snapshot
from rom2apk.icon import compose

CANVAS = 1024
BODY = 824     # Apple's icon grid: 824 px body on a 1024 px canvas
RADIUS = 185


def capture_at(core_path: Path, game_path: Path, seconds: float):
    """The frame shown `seconds` after power-on, and the display aspect ratio."""
    s = snapshot
    core = ctypes.CDLL(str(core_path))
    work_dir = ctypes.create_string_buffer(tempfile.mkdtemp(prefix="remasteria-icon-").encode())
    state = {"format": s.PIXEL_0RGB1555, "frame": None}

    def environment(cmd: int, data: int) -> bool:
        cmd &= ~s.ENV_EXPERIMENTAL
        if cmd == s.ENV_GET_CAN_DUPE:
            ctypes.cast(data, ctypes.POINTER(ctypes.c_bool))[0] = True
            return True
        if cmd in (s.ENV_GET_SYSTEM_DIRECTORY, s.ENV_GET_SAVE_DIRECTORY):
            ctypes.cast(data, ctypes.POINTER(ctypes.c_void_p))[0] = ctypes.addressof(work_dir)
            return True
        if cmd == s.ENV_SET_PIXEL_FORMAT:
            state["format"] = ctypes.cast(data, ctypes.POINTER(ctypes.c_int))[0]
            return True
        return False

    def video(data: int, width: int, height: int, pitch: int) -> None:
        if data:
            state["frame"] = (data, width, height, pitch)

    callbacks = (
        s.EnvironmentCb(environment), s.VideoCb(video), s.AudioCb(lambda left, right: None),
        s.AudioBatchCb(lambda data, frames: frames), s.InputPollCb(lambda: None), s.InputStateCb(lambda *args: 0),
    )
    core.retro_set_environment(callbacks[0])
    core.retro_init()
    core.retro_set_video_refresh(callbacks[1])
    core.retro_set_audio_sample(callbacks[2])
    core.retro_set_audio_sample_batch(callbacks[3])
    core.retro_set_input_poll(callbacks[4])
    core.retro_set_input_state(callbacks[5])

    content = game_path.read_bytes()
    buffer = ctypes.create_string_buffer(content, len(content))
    info = s.GameInfo(str(game_path).encode(), ctypes.cast(buffer, ctypes.c_void_p), len(content), None)
    core.retro_load_game.restype = ctypes.c_bool
    if not core.retro_load_game(ctypes.byref(info)):
        raise RuntimeError("o núcleo não conseguiu abrir o jogo")
    av = s.AvInfo()
    core.retro_get_system_av_info(ctypes.byref(av))
    for _ in range(int(seconds * (av.timing.fps or s.FPS_FALLBACK))):
        core.retro_run()
    if state["frame"] is None:
        raise RuntimeError("nenhum quadro foi gerado")
    data, width, height, pitch = state["frame"]
    frame = s.Frame(ctypes.string_at(data, pitch * height), width, height, pitch, state["format"])
    aspect = av.geometry.aspect_ratio if av.geometry.aspect_ratio > 0 else width / height
    return frame, aspect


def _coverage(x: float, y: float) -> float:
    """How much of the pixel centred at (x, y) lies inside the rounded body (2x2 supersampling)."""
    inside = 0
    for dx in (-0.25, 0.25):
        for dy in (-0.25, 0.25):
            px = abs(x + dx - CANVAS / 2) - (BODY / 2 - RADIUS)
            py = abs(y + dy - CANVAS / 2) - (BODY / 2 - RADIUS)
            if px <= 0 or py <= 0:
                inside += px <= RADIUS and py <= RADIUS
            else:
                inside += px * px + py * py <= RADIUS * RADIUS
    return inside / 4


def mac_icon(art) -> list:
    size = len(art)
    margin = (CANVAS - BODY) // 2
    rows = []
    for y in range(CANVAS):
        row = []
        for x in range(CANVAS):
            alpha = _coverage(x + 0.5, y + 0.5)
            if not alpha:
                row.append((0, 0, 0, 0))
                continue
            sx = min(max((x - margin) * size // BODY, 0), size - 1)
            sy = min(max((y - margin) * size // BODY, 0), size - 1)
            row.append((*art[sy][sx], round(alpha * 255)))
        rows.append(row)
    return rows


def main(argv) -> int:
    core, rom, seconds, out = Path(argv[1]), Path(argv[2]), float(argv[3]), Path(argv[4])
    frame, aspect = capture_at(core, rom, seconds) if seconds > 0 else snapshot.capture(core, rom)
    png.write(out, mac_icon(compose(frame.rgb(), aspect)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
