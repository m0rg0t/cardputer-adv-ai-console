from __future__ import annotations

import asyncio
import hashlib
import re
import shutil
import tempfile
from pathlib import Path
from urllib.parse import quote

import httpx

from .config import Settings

MARKDOWN_CODE_BLOCK = re.compile(r"```.*?```", re.DOTALL)
MARKDOWN_LINK = re.compile(r"\[([^]]+)]\([^)]+\)")
MARKDOWN_MARKS = re.compile(r"[*_>#]+")


class SpeechSynthesizer:
    def __init__(
        self,
        settings: Settings,
        transport: httpx.AsyncBaseTransport | None = None,
    ):
        self.settings = settings
        self.cache_root = settings.spool_root / "tts-cache"
        self.transport = transport
        self._resolved_elevenlabs_voice_id = ""

    def status(self) -> dict[str, str]:
        if not self.settings.tts_enabled:
            return {"status": "disabled", "detail": "TTS_ENABLED is false"}
        if not shutil.which(self.settings.ffmpeg_executable):
            return {"status": "error", "detail": "FFmpeg not found"}
        if self.settings.tts_provider == "macos":
            if not shutil.which(self.settings.say_executable):
                return {"status": "error", "detail": "macOS say not found"}
            return {
                "status": "ok",
                "detail": (
                    f"macOS {self.settings.tts_voice} {self.settings.tts_rate} wpm"
                ),
            }
        if self.settings.tts_provider == "elevenlabs":
            if not self.settings.elevenlabs_api_key:
                return {
                    "status": "error",
                    "detail": "ELEVENLABS_API_KEY is not configured",
                }
            if not self.settings.elevenlabs_voice:
                return {
                    "status": "error",
                    "detail": "ELEVENLABS_VOICE is not configured",
                }
            return {
                "status": "ok",
                "detail": (
                    "ElevenLabs "
                    f"{self.settings.tts_name or self.settings.elevenlabs_voice} / "
                    f"{self.settings.elevenlabs_model}"
                ),
            }
        return {
            "status": "error",
            "detail": f"Unsupported TTS_PROVIDER: {self.settings.tts_provider}",
        }

    async def synthesize(self, text: str) -> bytes:
        status = self.status()
        if status["status"] != "ok":
            raise RuntimeError(status["detail"])
        cleaned = self._clean(text)
        if not cleaned:
            raise RuntimeError("No readable text in the selected Codex response")
        identity = ":".join(
            (
                self.settings.tts_provider,
                self.settings.tts_name,
                self.settings.tts_voice,
                str(self.settings.tts_rate),
                self.settings.elevenlabs_voice,
                self.settings.elevenlabs_model,
                self.settings.elevenlabs_output_format,
                cleaned,
            )
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
            wav = temporary / "speech.wav"
            if self.settings.tts_provider == "elevenlabs":
                audio = temporary / "speech.mp3"
                audio.write_bytes(await self._synthesize_elevenlabs(cleaned))
            else:
                source = temporary / "speech.txt"
                audio = temporary / "speech.aiff"
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
                    str(audio),
                )
            await self._run(
                self.settings.ffmpeg_executable,
                "-nostdin",
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(audio),
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

    async def _synthesize_elevenlabs(self, text: str) -> bytes:
        voice_id = await self._elevenlabs_voice_id()
        url = f"https://api.elevenlabs.io/v1/text-to-speech/{quote(voice_id, safe='')}"
        try:
            async with httpx.AsyncClient(
                transport=self.transport,
                timeout=httpx.Timeout(90.0, connect=10.0),
            ) as client:
                response = await client.post(
                    url,
                    params={"output_format": self.settings.elevenlabs_output_format},
                    headers={
                        "xi-api-key": self.settings.elevenlabs_api_key,
                        "Content-Type": "application/json",
                        "Accept": ("audio/mpeg, audio/wav, application/octet-stream"),
                    },
                    json={
                        "text": text,
                        "model_id": self.settings.elevenlabs_model,
                    },
                )
        except httpx.HTTPError as error:
            raise RuntimeError(f"ElevenLabs request failed: {error}") from error
        if response.status_code >= 400:
            detail = self._http_error(response)
            raise RuntimeError(f"ElevenLabs synthesis failed: {detail}")
        if not response.content:
            raise RuntimeError("ElevenLabs returned empty audio")
        return response.content

    async def _elevenlabs_voice_id(self) -> str:
        if self._resolved_elevenlabs_voice_id:
            return self._resolved_elevenlabs_voice_id
        configured = self.settings.elevenlabs_voice.strip()
        if re.fullmatch(r"[A-Za-z0-9_-]{16,64}", configured):
            self._resolved_elevenlabs_voice_id = configured
            return configured

        try:
            async with httpx.AsyncClient(
                transport=self.transport,
                timeout=httpx.Timeout(30.0, connect=10.0),
            ) as client:
                response = await client.get(
                    "https://api.elevenlabs.io/v2/voices",
                    params={
                        "search": configured,
                        "page_size": "100",
                        "include_total_count": "false",
                    },
                    headers={"xi-api-key": self.settings.elevenlabs_api_key},
                )
        except httpx.HTTPError as error:
            raise RuntimeError(
                f"ElevenLabs voice lookup request failed: {error}"
            ) from error
        if response.status_code >= 400:
            detail = self._http_error(response)
            raise RuntimeError(f"ElevenLabs voice lookup failed: {detail}")
        try:
            voices = response.json().get("voices", [])
        except ValueError as error:
            raise RuntimeError("ElevenLabs returned invalid voice data") from error
        match = next(
            (
                item
                for item in voices
                if str(item.get("name", "")).casefold() == configured.casefold()
            ),
            None,
        )
        if not match or not match.get("voice_id"):
            raise RuntimeError(
                f"ElevenLabs voice not found in this account: {configured}"
            )
        self._resolved_elevenlabs_voice_id = str(match["voice_id"])
        return self._resolved_elevenlabs_voice_id

    @staticmethod
    def _http_error(response: httpx.Response) -> str:
        try:
            payload = response.json()
            detail = payload.get("detail", payload)
        except ValueError:
            detail = response.text
        return f"HTTP {response.status_code}: {str(detail)[:300]}"

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
