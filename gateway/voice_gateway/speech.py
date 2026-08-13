from __future__ import annotations

import asyncio
import hashlib
import re
import shutil
import tempfile
from pathlib import Path

from .config import Settings


MARKDOWN_CODE_BLOCK = re.compile(r"```.*?```", re.DOTALL)
MARKDOWN_LINK = re.compile(r"\[([^]]+)]\([^)]+\)")
MARKDOWN_MARKS = re.compile(r"[*_>#]+")


class SpeechSynthesizer:
    def __init__(self, settings: Settings):
        self.settings = settings
        self.cache_root = settings.spool_root / "tts-cache"

    def status(self) -> dict[str, str]:
        if not self.settings.tts_enabled:
            return {"status": "disabled", "detail": "TTS_ENABLED is false"}
        if not shutil.which(self.settings.say_executable):
            return {"status": "error", "detail": "macOS say not found"}
        if not shutil.which(self.settings.ffmpeg_executable):
            return {"status": "error", "detail": "FFmpeg not found"}
        return {
            "status": "ok",
            "detail": f"{self.settings.tts_voice} {self.settings.tts_rate} wpm",
        }

    async def synthesize(self, text: str) -> bytes:
        status = self.status()
        if status["status"] != "ok":
            raise RuntimeError(status["detail"])
        cleaned = self._clean(text)
        if not cleaned:
            raise RuntimeError("No readable text in the selected Codex response")
        identity = (
            f"{self.settings.tts_voice}:{self.settings.tts_rate}:{cleaned}"
        ).encode("utf-8")
        digest = hashlib.sha256(identity).hexdigest()
        self.cache_root.mkdir(parents=True, exist_ok=True)
        cached = self.cache_root / f"{digest}.wav"
        if cached.is_file():
            return cached.read_bytes()

        with tempfile.TemporaryDirectory(
            prefix="cardputer-tts-", dir=self.settings.spool_root
        ) as temporary_name:
            temporary = Path(temporary_name)
            source = temporary / "speech.txt"
            aiff = temporary / "speech.aiff"
            wav = temporary / "speech.wav"
            source.write_text(cleaned, encoding="utf-8")
            await self._run(
                self.settings.say_executable,
                "-v",
                self.settings.tts_voice,
                "-r",
                str(self.settings.tts_rate),
                "-f",
                str(source),
                "-o",
                str(aiff),
            )
            await self._run(
                self.settings.ffmpeg_executable,
                "-nostdin",
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(aiff),
                "-ac",
                "1",
                "-ar",
                "16000",
                "-c:a",
                "pcm_s16le",
                str(wav),
            )
            data = wav.read_bytes()
        pending = cached.with_suffix(".wav.tmp")
        pending.write_bytes(data)
        pending.replace(cached)
        self._prune_cache()
        return data

    def _clean(self, text: str) -> str:
        text = MARKDOWN_CODE_BLOCK.sub(" Фрагмент кода пропущен. ", text)
        text = MARKDOWN_LINK.sub(r"\1", text)
        text = text.replace("`", "")
        text = MARKDOWN_MARKS.sub("", text)
        text = re.sub(r"\s+", " ", text).strip()
        return text[: max(200, min(self.settings.tts_max_chars, 12000))]

    async def _run(self, *command: str) -> None:
        executable = shutil.which(command[0])
        if not executable:
            raise RuntimeError(f"Executable not found: {command[0]}")
        process = await asyncio.create_subprocess_exec(
            executable,
            *command[1:],
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
        try:
            _, stderr = await asyncio.wait_for(process.communicate(), timeout=90)
        except TimeoutError as error:
            process.kill()
            await process.wait()
            raise RuntimeError("Text-to-speech timed out") from error
        if process.returncode != 0:
            detail = stderr.decode("utf-8", "replace").strip()[-400:]
            raise RuntimeError(f"Text-to-speech failed: {detail}")

    def _prune_cache(self) -> None:
        cached = sorted(
            self.cache_root.glob("*.wav"),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
        for stale in cached[8:]:
            stale.unlink(missing_ok=True)
