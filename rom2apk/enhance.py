"""Video profiles: which shader the app starts with and which core options improve the image."""

from __future__ import annotations

from typing import Dict, Optional

from .consoles import Console

VIDEO_PROFILES = ("hd", "sharp", "crt", "lcd", "original")

# Internal-resolution multipliers each 3D core can actually render at.
MAX_SCALE = {"psx": 2, "n64": 4}
DEFAULT_SCALE = {"psx": 2, "n64": 3}

_N64_SCREEN_SIZES = {1: "320x240", 2: "640x480", 3: "960x720", 4: "1280x960"}


def resolve_scale(console: Console, requested: Optional[int]) -> int:
    if not console.is_3d:
        return 1
    scale = requested or DEFAULT_SCALE[console.id]
    return max(1, min(scale, MAX_SCALE[console.id]))


def initial_shader(console: Console, video: str) -> str:
    # 3D consoles get real extra pixels from the core; an edge-smoothing shader on top
    # of that only blurs the result, so plain bilinear is used instead.
    if video == "hd" and console.is_3d:
        return "original"
    return video


def core_variables(console: Console, video: str, scale: int) -> Dict[str, str]:
    enhanced = video != "original"
    variables: Dict[str, str] = {}

    if console.id == "nes" and enhanced:
        variables["fceumm_nospritelimit"] = "enabled"

    elif console.id in ("md", "sms", "gg") and enhanced:
        variables["genesis_plus_gx_no_sprite_limit"] = "enabled"

    elif console.id == "gb":
        variables["gambatte_gb_colorization"] = "auto" if enhanced else "disabled"

    elif console.id == "psx":
        variables["pcsx_rearmed_neon_enhancement_enable"] = "enabled" if scale >= 2 else "disabled"
        variables["pcsx_rearmed_neon_enhancement_no_main"] = "enabled" if scale >= 2 else "disabled"

    elif console.id == "n64":
        variables["mupen64plus-rdp-plugin"] = "gliden64"
        variables["mupen64plus-cpucore"] = "dynamic_recompiler"
        variables["mupen64plus-43screensize"] = _N64_SCREEN_SIZES[scale]
        variables["mupen64plus-BilinearMode"] = "3point" if enhanced else "standard"
        variables["mupen64plus-pak1"] = "memory"

    return variables
