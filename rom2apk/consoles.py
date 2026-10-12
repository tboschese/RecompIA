from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass(frozen=True)
class Console:
    id: str
    name: str
    core: str
    extensions: tuple
    is_3d: bool = False

    @property
    def core_library(self) -> str:
        return f"lib{self.core}_libretro_android.so"


CONSOLES = {
    c.id: c
    for c in [
        Console("nes", "Nintendo (NES)", "fceumm", (".nes", ".fds", ".unf", ".unif")),
        Console("snes", "Super Nintendo", "snes9x", (".sfc", ".smc")),
        Console("gb", "Game Boy", "gambatte", (".gb",)),
        Console("gbc", "Game Boy Color", "gambatte", (".gbc",)),
        Console("gba", "Game Boy Advance", "mgba", (".gba",)),
        Console("md", "Mega Drive / Genesis", "genesis_plus_gx", (".md", ".gen", ".smd")),
        Console("sms", "Master System", "genesis_plus_gx", (".sms",)),
        Console("gg", "Game Gear", "genesis_plus_gx", (".gg",)),
        Console("psx", "PlayStation", "pcsx_rearmed", (".cue", ".chd", ".pbp", ".exe"), is_3d=True),
        Console("n64", "Nintendo 64", "mupen64plus_next_gles3", (".n64", ".z64", ".v64"), is_3d=True),
    ]
}

_MAX_MEGADRIVE_BIN = 8 * 1024 * 1024


class DetectionError(Exception):
    pass


def supported_extensions() -> set:
    exts = {ext for console in CONSOLES.values() for ext in console.extensions}
    return exts | {".bin"}


def detect_console(rom: Path, forced: Optional[str] = None) -> Console:
    if forced:
        if forced not in CONSOLES:
            raise DetectionError(f"Console desconhecido '{forced}'. Opções: {', '.join(CONSOLES)}")
        return CONSOLES[forced]

    ext = rom.suffix.lower()
    for console in CONSOLES.values():
        if ext in console.extensions:
            return console

    if ext == ".bin":
        if rom.with_suffix(".cue").exists() or rom.with_suffix(".CUE").exists():
            raise DetectionError(
                f"'{rom.name}' parece ser uma faixa de CD. Passe o arquivo .cue em vez do .bin."
            )
        if rom.stat().st_size <= _MAX_MEGADRIVE_BIN:
            return CONSOLES["md"]
        raise DetectionError("Não foi possível identificar o console deste .bin. Use --console.")

    raise DetectionError(
        f"Extensão '{ext}' não suportada. Use --console ou uma destas: "
        + ", ".join(sorted(supported_extensions()))
    )
