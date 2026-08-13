from __future__ import annotations

import argparse
import os
import sqlite3
import subprocess
from pathlib import Path


def add_audio_reference(note_path: Path, audio_path: Path) -> None:
    text = note_path.read_text(encoding="utf-8")
    embed = f"![[{audio_path.name}]]"
    if embed in text:
        return
    lines = text.splitlines()
    audio_field = f'audio_file: "{audio_path.name}"'
    for index, line in enumerate(lines):
        if line.startswith("audio_file:"):
            lines[index] = audio_field
            break
    else:
        insert_at = next(
            (
                index + 1
                for index, line in enumerate(lines)
                if line.startswith("audio_sha256:")
            ),
            1,
        )
        lines.insert(insert_at, audio_field)
    lines.extend(["", "## Audio", "", embed, ""])
    temporary = note_path.with_suffix(".md.tmp")
    temporary.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.replace(temporary, note_path)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Export retained Cardputer WAV files beside existing notes."
    )
    parser.add_argument("database", type=Path)
    parser.add_argument("--app-root", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()

    with sqlite3.connect(args.database) as connection:
        rows = connection.execute(
            "SELECT audio_path, note_path FROM voice_jobs "
            "WHERE status = 'completed' AND note_path != ''"
        ).fetchall()

    converted = 0
    skipped = 0
    for source_value, note_value in rows:
        source = Path(source_value)
        if not source.is_absolute():
            source = args.app_root / source
        note_path = Path(note_value)
        if not source.is_file() or not note_path.is_file():
            print(f"SKIP missing source or note: {source} -> {note_path}")
            skipped += 1
            continue
        audio_path = note_path.with_suffix(".mp3")
        if not audio_path.is_file():
            temporary = audio_path.with_suffix(".mp3.tmp")
            temporary.unlink(missing_ok=True)
            result = subprocess.run(
                [
                    args.ffmpeg,
                    "-nostdin",
                    "-y",
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-i",
                    str(source),
                    "-vn",
                    "-codec:a",
                    "libmp3lame",
                    "-b:a",
                    "64k",
                    "-f",
                    "mp3",
                    str(temporary),
                ],
                capture_output=True,
                check=False,
                text=True,
            )
            if result.returncode != 0:
                temporary.unlink(missing_ok=True)
                print(f"ERROR {note_path.name}: {result.stderr.strip()}")
                skipped += 1
                continue
            os.replace(temporary, audio_path)
            converted += 1
        add_audio_reference(note_path, audio_path)
        print(f"OK {note_path.name} -> {audio_path.name}")
    print(f"Converted: {converted}; skipped: {skipped}; total: {len(rows)}")
    return 1 if skipped else 0


if __name__ == "__main__":
    raise SystemExit(main())
