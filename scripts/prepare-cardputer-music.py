#!/usr/bin/env python3
"""Prepare the Строка music library for simple SD-card MP3 players."""

from __future__ import annotations

import argparse
import subprocess
import unicodedata
from pathlib import Path


ARTIST = "Строка"
GENRE = "Folk"
INVALID_FAT_CHARS = '<>:"/\\|?*'
TRANSLITERATION = str.maketrans(
    {
        "А": "A", "Б": "B", "В": "V", "Г": "G", "Д": "D", "Е": "E",
        "Ё": "Yo", "Ж": "Zh", "З": "Z", "И": "I", "Й": "Y", "К": "K",
        "Л": "L", "М": "M", "Н": "N", "О": "O", "П": "P", "Р": "R",
        "С": "S", "Т": "T", "У": "U", "Ф": "F", "Х": "Kh", "Ц": "Ts",
        "Ч": "Ch", "Ш": "Sh", "Щ": "Shch", "Ъ": "", "Ы": "Y", "Ь": "",
        "Э": "E", "Ю": "Yu", "Я": "Ya",
        "а": "a", "б": "b", "в": "v", "г": "g", "д": "d", "е": "e",
        "ё": "yo", "ж": "zh", "з": "z", "и": "i", "й": "y", "к": "k",
        "л": "l", "м": "m", "н": "n", "о": "o", "п": "p", "р": "r",
        "с": "s", "т": "t", "у": "u", "ф": "f", "х": "kh", "ц": "ts",
        "ч": "ch", "ш": "sh", "щ": "shch", "ъ": "", "ы": "y", "ь": "",
        "э": "e", "ю": "yu", "я": "ya",
    }
)


def normalized(value: str) -> str:
    value = unicodedata.normalize("NFC", value).strip()
    return "".join("_" if character in INVALID_FAT_CHARS else character for character in value)


def ascii_filename(value: str) -> str:
    value = normalized(value).translate(TRANSLITERATION)
    safe = [character if character.isascii() and character.isalnum() else "_" for character in value]
    return "_".join(part for part in "".join(safe).split("_") if part)


def find_cover(original_album: Path) -> Path | None:
    candidates = [
        path
        for path in original_album.iterdir()
        if path.is_file() and path.suffix.lower() in {".jpg", ".jpeg", ".png"}
    ]
    if not candidates:
        return None

    def cover_rank(path: Path) -> tuple[int, str]:
        name = path.name.casefold()
        preferred = any(word in name for word in ("cover", "облож", "птичка-певчая"))
        generated = any(word in name for word in ("gemini", "chatgpt", "replicate"))
        return (0 if preferred else 1, 1 if generated else 0, name)

    return sorted(candidates, key=cover_rank)[0]


def run_ffmpeg(
    source: Path,
    destination: Path,
    cover: Path | None,
    album: str,
    title: str,
    track_number: int,
    track_total: int,
) -> None:
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(source)]
    if cover:
        command.extend(["-i", str(cover), "-map", "0:a:0", "-map", "1:v:0"])
    else:
        command.extend(["-map", "0:a:0"])

    command.extend(["-map_metadata", "-1", "-c:a", "copy"])
    if cover:
        command.extend(
            [
                "-c:v",
                "mjpeg",
                "-vf",
                "scale=500:500:force_original_aspect_ratio=decrease",
                "-disposition:v:0",
                "attached_pic",
            ]
        )

    command.extend(
        [
            "-id3v2_version",
            "3",
            "-metadata",
            f"artist={ARTIST}",
            "-metadata",
            f"album_artist={ARTIST}",
            "-metadata",
            f"album={album}",
            "-metadata",
            f"title={title}",
            "-metadata",
            f"track={track_number}/{track_total}",
            "-metadata",
            f"genre={GENRE}",
            "-metadata",
            "comment=Prepared for Cardputer ADV",
            str(destination),
        ]
    )
    subprocess.run(command, check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, help="PLAYER backup music-mp3 directory")
    parser.add_argument("originals", type=Path, help="Original album directory root")
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    args.destination.mkdir(parents=True, exist_ok=True)
    albums = sorted(
        (path for path in args.source.iterdir() if path.is_dir()),
        key=lambda path: normalized(path.name).casefold(),
    )

    written = 0
    for album_path in albums:
        album = normalized(album_path.name)
        tracks = sorted(
            album_path.glob("*.mp3"), key=lambda path: normalized(path.stem).casefold()
        )
        cover = find_cover(args.originals / album_path.name)
        for index, source in enumerate(tracks, start=1):
            title = normalized(source.stem)
            filename = f"{ascii_filename(f'{album} {index:02d} {title}')}.mp3"
            run_ffmpeg(
                source,
                args.destination / filename,
                cover,
                album,
                title,
                index,
                len(tracks),
            )
            written += 1
            print(filename)

    print(f"Prepared {written} tracks in {args.destination}")


if __name__ == "__main__":
    main()
