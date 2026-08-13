#!/usr/bin/env python3
"""Create a private gateway .env without committing personal paths or tokens."""

from __future__ import annotations

import argparse
import json
import os
import secrets
import shlex
import sys
from pathlib import Path


GATEWAY_DIR = Path(__file__).resolve().parent.parent
EXAMPLE_ENV = GATEWAY_DIR / ".env.example"


def discover_obsidian_vaults() -> list[Path]:
    """Read Obsidian's own registry and common local markers."""
    found: set[Path] = set()
    registry = Path.home() / "Library/Application Support/obsidian/obsidian.json"
    try:
        data = json.loads(registry.read_text(encoding="utf-8"))
        for item in data.get("vaults", {}).values():
            raw_path = item.get("path") if isinstance(item, dict) else None
            if raw_path:
                candidate = Path(raw_path).expanduser()
                if candidate.is_dir():
                    found.add(candidate.resolve())
    except (OSError, ValueError, TypeError):
        pass

    common_roots = [
        Path.home() / "Documents",
        Path.home() / "Obsidian",
        Path.home() / "Library/Mobile Documents/iCloud~md~obsidian/Documents",
    ]
    for root in common_roots:
        if not root.is_dir():
            continue
        if (root / ".obsidian").is_dir():
            found.add(root.resolve())
        try:
            for marker in root.glob("*/.obsidian"):
                if marker.is_dir():
                    found.add(marker.parent.resolve())
        except OSError:
            pass
    return sorted(found, key=lambda path: str(path).lower())


def parse_env(lines: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or "=" not in stripped:
            continue
        key, value = stripped.split("=", 1)
        try:
            parsed = shlex.split(value, comments=False, posix=True)
            values[key] = parsed[0] if parsed else ""
        except ValueError:
            values[key] = value
    return values


def render_env(template: list[str], values: dict[str, str]) -> str:
    rendered: list[str] = []
    seen: set[str] = set()
    for line in template:
        stripped = line.strip()
        if stripped and not stripped.startswith("#") and "=" in stripped:
            key = stripped.split("=", 1)[0]
            if key in values:
                line = f"{key}={shlex.quote(values[key])}\n"
                seen.add(key)
        rendered.append(line)
    for key in sorted(values.keys() - seen):
        rendered.append(f"{key}={shlex.quote(values[key])}\n")
    return "".join(rendered)


def prompt(label: str, default: str) -> str:
    answer = input(f"{label} [{default}]: ").strip()
    return answer or default


def prompt_bool(label: str, default: bool) -> bool:
    marker = "Y/n" if default else "y/N"
    answer = input(f"{label} [{marker}]: ").strip().lower()
    return default if not answer else answer in {"y", "yes", "1", "true"}


def select_vault(candidates: list[Path]) -> Path:
    if candidates:
        print("Detected Obsidian vaults:")
        for index, path in enumerate(candidates, 1):
            print(f"  {index}. {path}")
        raw = input("Select a number or enter another absolute path [1]: ").strip()
        if not raw:
            return candidates[0]
        if raw.isdigit() and 1 <= int(raw) <= len(candidates):
            return candidates[int(raw) - 1]
        return Path(raw).expanduser()
    return Path(input("Absolute path to your Obsidian Vault: ").strip()).expanduser()


def validate_vault(vault: Path) -> Path:
    resolved = vault.expanduser().resolve()
    if not resolved.is_dir():
        raise ValueError(f"Vault directory does not exist: {resolved}")
    if not os.access(resolved, os.W_OK):
        raise ValueError(f"Vault directory is not writable: {resolved}")
    if not (resolved / ".obsidian").exists():
        print(
            f"Warning: {resolved} has no .obsidian directory; it will still be used.",
            file=sys.stderr,
        )
    return resolved


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vault", type=Path, help="absolute Obsidian Vault path")
    parser.add_argument("--notes-folder", default="Inbox/Cardputer Voice")
    parser.add_argument("--env-file", type=Path, default=GATEWAY_DIR / ".env")
    parser.add_argument("--non-interactive", action="store_true")
    args = parser.parse_args()

    if args.non_interactive and args.vault is None:
        parser.error("--non-interactive requires --vault")

    env_path = args.env_file.expanduser().resolve()
    template = EXAMPLE_ENV.read_text(encoding="utf-8").splitlines(keepends=True)
    current_lines = (
        env_path.read_text(encoding="utf-8").splitlines(keepends=True)
        if env_path.exists()
        else template
    )
    values = parse_env(current_lines)

    vault = args.vault or select_vault(discover_obsidian_vaults())
    try:
        vault = validate_vault(vault)
    except ValueError as error:
        parser.error(str(error))

    values["OBSIDIAN_VAULT_ROOT"] = str(vault)
    values["OBSIDIAN_NOTES_FOLDER"] = (
        args.notes_folder
        if args.non_interactive
        else prompt("Folder inside the Vault", args.notes_folder)
    )
    if not values.get("VOICE_DEVICE_TOKEN") or values["VOICE_DEVICE_TOKEN"].startswith(
        "replace-"
    ):
        values["VOICE_DEVICE_TOKEN"] = secrets.token_urlsafe(32)

    if not args.non_interactive:
        provider = prompt(
            "Transcription provider (mock/whisper-server/faster-whisper/openai)",
            values.get("TRANSCRIPTION_PROVIDER", "whisper-server"),
        )
        values["TRANSCRIPTION_PROVIDER"] = provider
        values["CODEX_ENABLED"] = str(
            prompt_bool("Enable local Codex tasks", values.get("CODEX_ENABLED") == "true")
        ).lower()
        values["TTS_ENABLED"] = str(
            prompt_bool("Enable local text-to-speech", values.get("TTS_ENABLED") == "true")
        ).lower()

    env_path.parent.mkdir(parents=True, exist_ok=True)
    env_path.write_text(render_env(template, values), encoding="utf-8")
    env_path.chmod(0o600)
    print(f"Wrote private configuration to {env_path}")
    print(f"Obsidian notes will be stored in {vault / values['OBSIDIAN_NOTES_FOLDER']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
