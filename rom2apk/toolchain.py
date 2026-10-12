"""Locates the JDK / Android SDK and manages the signing keystore."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from .paths import home

REQUIRED_PLATFORM = "android-35"


class ToolchainError(Exception):
    pass


@dataclass
class Keystore:
    path: Path
    store_password: str
    alias: str
    key_password: str


def find_android_sdk() -> Path:
    candidates = [
        os.environ.get("ANDROID_HOME"),
        os.environ.get("ANDROID_SDK_ROOT"),
        str(Path.home() / "Library/Android/sdk"),
        str(Path.home() / "Android/Sdk"),
        "/opt/homebrew/share/android-commandlinetools",
    ]
    for candidate in candidates:
        if candidate and (Path(candidate) / "platforms").is_dir():
            return Path(candidate)
    raise ToolchainError(
        "Android SDK não encontrado. Instale o Android Studio ou defina ANDROID_HOME."
    )


def find_java_home() -> Path:
    env = os.environ.get("JAVA_HOME")
    if env and _java_major(Path(env)) >= 17:
        return Path(env)

    if Path("/usr/libexec/java_home").exists():
        result = subprocess.run(
            ["/usr/libexec/java_home", "-v", "17+"], capture_output=True, text=True
        )
        if result.returncode == 0 and result.stdout.strip():
            return Path(result.stdout.strip())

    java = shutil.which("java")
    if java:
        java_home = Path(java).resolve().parent.parent
        if _java_major(java_home) >= 17:
            return java_home

    raise ToolchainError("É necessário o JDK 17 ou mais recente. Instale com: brew install openjdk@17")


def _java_major(java_home: Path) -> int:
    java = java_home / "bin" / "java"
    if not java.exists():
        return 0
    result = subprocess.run([str(java), "-version"], capture_output=True, text=True)
    match = re.search(r'version "(\d+)', result.stderr)
    return int(match.group(1)) if match else 0


def check(sdk: Path) -> List[str]:
    problems = []
    if not (sdk / "platforms" / REQUIRED_PLATFORM).is_dir():
        problems.append(
            f"Plataforma {REQUIRED_PLATFORM} ausente. Instale com: "
            f"sdkmanager 'platforms;{REQUIRED_PLATFORM}'"
        )
    if not any((sdk / "build-tools").glob("3[5-9]*")):
        problems.append("build-tools 35+ ausente. Instale com: sdkmanager 'build-tools;35.0.0'")
    return problems


def default_keystore(java_home: Path) -> Keystore:
    keystore = Keystore(home() / "rom2apk.keystore", "rom2apk", "rom2apk", "rom2apk")
    if not keystore.path.exists():
        subprocess.run(
            [
                str(java_home / "bin" / "keytool"),
                "-genkeypair", "-noprompt",
                "-keystore", str(keystore.path),
                "-storetype", "PKCS12",
                "-alias", keystore.alias,
                "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
                "-storepass", keystore.store_password,
                "-keypass", keystore.key_password,
                "-dname", "CN=rom2apk, O=rom2apk",
            ],
            check=True,
            capture_output=True,
        )
    return keystore


def custom_keystore(path: str, store_password: str, alias: str, key_password: Optional[str]) -> Keystore:
    keystore_path = Path(path).expanduser().resolve()
    if not keystore_path.exists():
        raise ToolchainError(f"Keystore não encontrado: {keystore_path}")
    return Keystore(keystore_path, store_password, alias, key_password or store_password)
