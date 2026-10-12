"""Turns the user's ROM into the set of files shipped inside the APK."""

from __future__ import annotations

import hashlib
import re
import shutil
import tempfile
import unicodedata
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

from .consoles import Console, DetectionError, detect_console, supported_extensions

_CUE_FILE_LINE = re.compile(r'^(\s*FILE\s+)(?:"([^"]+)"|(\S+))(\s+\S+\s*)$', re.IGNORECASE)
_TITLE_TAGS = re.compile(r"\s*[\(\[][^\)\]]*[\)\]]")


@dataclass
class PreparedGame:
    console: Console
    title: str
    main: str
    files: Dict[str, Path] = field(default_factory=dict)
    version: str = ""
    _tempdir: Optional[tempfile.TemporaryDirectory] = None

    def cleanup(self) -> None:
        if self._tempdir is not None:
            self._tempdir.cleanup()


def title_from_filename(path: Path) -> str:
    title = _TITLE_TAGS.sub("", path.stem).replace("_", " ").strip()
    return re.sub(r"\s{2,}", " ", title) or path.stem


def slugify(text: str) -> str:
    ascii_text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    slug = re.sub(r"[^a-z0-9]+", "", ascii_text.lower())
    if not slug or not slug[0].isalpha():
        slug = "game" + slug
    return slug[:40]


def prepare(rom: Path, forced_console: Optional[str], title: Optional[str]) -> PreparedGame:
    tempdir = tempfile.TemporaryDirectory(prefix="rom2apk-")
    work = Path(tempdir.name)
    source = rom

    if rom.suffix.lower() == ".zip":
        source = _extract_zip(rom, work / "zip")

    console = detect_console(source, forced_console)
    game = PreparedGame(console=console, title=title or title_from_filename(rom), main="", _tempdir=tempdir)

    ext = source.suffix.lower()
    if ext == ".cue":
        game.main = "game.cue"
        game.files = _prepare_cue(source, work / "game.cue")
    else:
        game.main = "game" + ext
        game.files = {game.main: source}

    game.version = _fingerprint(game)
    return game


def _extract_zip(archive: Path, target: Path) -> Path:
    with zipfile.ZipFile(archive) as zf:
        names = [n for n in zf.namelist() if not n.endswith("/")]
        known = supported_extensions()
        cues = [n for n in names if n.lower().endswith(".cue")]
        candidates = cues or [n for n in names if Path(n).suffix.lower() in known]
        if not candidates:
            raise DetectionError(f"Nenhuma ROM reconhecida dentro de '{archive.name}'.")
        zf.extractall(target)
    return target / candidates[0]


def _prepare_cue(cue: Path, rewritten: Path) -> Dict[str, Path]:
    """Renames tracks to ASCII-safe names and rewrites the cue sheet to match."""
    files: Dict[str, Path] = {}
    lines: List[str] = []
    for line in cue.read_text(errors="replace").splitlines():
        match = _CUE_FILE_LINE.match(line)
        if not match:
            lines.append(line)
            continue
        original = match.group(2) or match.group(3)
        track = cue.parent / original
        if not track.exists():
            raise DetectionError(f"O .cue referencia '{original}', mas o arquivo não foi encontrado.")
        new_name = f"track{len(files) + 1:02d}{track.suffix.lower()}"
        files[new_name] = track
        lines.append(f'{match.group(1)}"{new_name}"{match.group(4)}')

    if not files:
        raise DetectionError(f"Nenhuma faixa encontrada em '{cue.name}'.")
    rewritten.write_text("\n".join(lines) + "\n")
    return {"game.cue": rewritten, **files}


def _fingerprint(game: PreparedGame) -> str:
    digest = hashlib.sha1(game.console.core.encode())
    for name, path in sorted(game.files.items()):
        digest.update(name.encode())
        with path.open("rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                digest.update(chunk)
    return digest.hexdigest()[:16]


def copy_files(game: PreparedGame, target_dir: Path) -> None:
    target_dir.mkdir(parents=True, exist_ok=True)
    for name, path in game.files.items():
        shutil.copyfile(path, target_dir / name)
