from __future__ import annotations

import asyncio
import hashlib
import io
import os
import re
import shutil
import tempfile
import uuid
import wave
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
        # A cache miss can be served by several Cardputer requests at once
        # (for example, when a user retries Read).  Serialise cache misses so
        # we do not synthesize the same response repeatedly, while the unique
        # temporary path below also keeps an interrupted writer from exposing
        # a partial WAV to another request.
        self._cache_lock = asyncio.Lock()

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
            data = self._read_cached_wav(cached)
            if data:
                return data

        async with self._cache_lock:
            # Another request may have filled the cache while this request
            # waited for the lock.
            if cached.is_file():
                data = self._read_cached_wav(cached)
                if data:
                    return data

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
                try:
                    data = wav.read_bytes()
                except OSError as error:
                    raise RuntimeError("Text-to-speech produced no WAV output") from error
                if not self._is_valid_wav(data):
                    raise RuntimeError("Text-to-speech produced invalid WAV")

            # The previous fixed ``.wav.tmp`` name allowed two requests to
            # replace/remove each other's temporary file.  A unique sibling
            # plus os.replace gives readers an all-or-nothing cache entry.
            pending = self.cache_root / (
                f".{cached.name}.{uuid.uuid4().hex}.tmp"
            )
            try:
                pending.write_bytes(data)
                os.replace(pending, cached)
            except OSError:
                # Caching is an optimisation; a full/read-only cache must not
                # turn an otherwise valid speech response into a 503.
                pending.unlink(missing_ok=True)
            self._prune_cache()
            return data

    @classmethod
    def _is_valid_wav(cls, data: bytes) -> bool:
        """Accept only the PCM shape the Cardputer playback path supports."""
        try:
            with wave.open(io.BytesIO(data), "rb") as wav:
                channels = wav.getnchannels()
                sample_width = wav.getsampwidth()
                frames = wav.getnframes()
                if (
                    channels != 1
                    or sample_width != 2
                    or not 8000 <= wav.getframerate() <= 48000
                    or frames <= 0
                ):
                    return False
                bytes_per_frame = channels * sample_width
                expected_audio_bytes = frames * bytes_per_frame
                read_audio_bytes = 0
                remaining_frames = frames
                while remaining_frames > 0:
                    chunk = wav.readframes(min(remaining_frames, 4096))
                    if not chunk or len(chunk) % bytes_per_frame != 0:
                        return False
                    read_audio_bytes += len(chunk)
                    frames_read = len(chunk) // bytes_per_frame
                    if frames_read == 0:
                        return False
                    remaining_frames -= frames_read
                return (
                    remaining_frames == 0
                    and read_audio_bytes == expected_audio_bytes
                )
        except (wave.Error, EOFError, OSError, ValueError):
            return False

    @classmethod
    def _read_cached_wav(cls, path: Path) -> bytes:
        try:
            data = path.read_bytes()
        except OSError:
            return b""
        if cls._is_valid_wav(data):
            return data
        # A stale cache entry from an older gateway version must not be served
        # to the device forever.  Failure to remove it is harmless; the next
        # synthesis can still replace it atomically.
        try:
            path.unlink(missing_ok=True)
        except OSError:
            pass
        return b""

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
            payload = response.json()
        except (ValueError, UnicodeError, TypeError) as error:
            raise RuntimeError("ElevenLabs returned invalid voice data") from error
        if not isinstance(payload, dict):
            raise RuntimeError("ElevenLabs returned invalid voice data")
        voices = payload.get("voices", [])
        if not isinstance(voices, list):
            raise RuntimeError("ElevenLabs returned invalid voice data")
        if any(not isinstance(item, dict) for item in voices):
            raise RuntimeError("ElevenLabs returned invalid voice data")
        match = next(
            (
                item
                for item in voices
                if str(item.get("name", "")).casefold() == configured.casefold()
            ),
            None,
        )
        voice_id = match.get("voice_id") if match else None
        if not voice_id:
            raise RuntimeError(
                f"ElevenLabs voice not found in this account: {configured}"
            )
        self._resolved_elevenlabs_voice_id = str(voice_id)
        return self._resolved_elevenlabs_voice_id

    @staticmethod
    def _http_error(response: httpx.Response) -> str:
        try:
            payload = response.json()
            detail = payload.get("detail", payload) if isinstance(payload, dict) else payload
        except (ValueError, UnicodeError, TypeError):
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
            if process.returncode is None:
                try:
                    process.kill()
                except OSError:
                    pass
            try:
                await asyncio.wait_for(process.wait(), timeout=5)
            except (TimeoutError, OSError):
                pass
            raise RuntimeError("Text-to-speech timed out") from error
        except asyncio.CancelledError:
            if process.returncode is None:
                try:
                    process.kill()
                except OSError:
                    pass
            try:
                await asyncio.wait_for(process.wait(), timeout=5)
            except (TimeoutError, OSError):
                pass
            raise
        if process.returncode != 0:
            detail = stderr.decode("utf-8", "replace").strip()[-400:]
            raise RuntimeError(f"Text-to-speech failed: {detail}")

    def _prune_cache(self) -> None:
        entries: list[tuple[float, Path]] = []
        for path in self.cache_root.glob("*.wav"):
            try:
                entries.append((path.stat().st_mtime, path))
            except OSError:
                continue
        entries.sort(key=lambda item: item[0], reverse=True)
        for _, stale in entries[8:]:
            try:
                stale.unlink(missing_ok=True)
            except OSError:
                continue
