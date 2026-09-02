from __future__ import annotations

import asyncio
from pathlib import Path
import tempfile
from typing import Any
from urllib.parse import urlparse
import wave

import httpx

from .config import Settings


class Transcriber:
    def __init__(self, settings: Settings):
        self.settings = settings
        self._whisper_model: Any | None = None
        self._whisper_lock = asyncio.Lock()

    async def transcribe(self, wav_path: Path, original_filename: str) -> str:
        provider = self.settings.transcription_provider
        if provider == "mock":
            return f"Test transcription for {original_filename}."
        if provider == "openai":
            return await self._openai(wav_path, original_filename)
        if provider == "faster-whisper":
            return await self._faster_whisper(wav_path)
        if provider == "whisper-server":
            return await self._whisper_server(wav_path, original_filename)
        raise RuntimeError(f"Unsupported transcription provider: {provider}")

    async def status(self) -> dict[str, str]:
        provider = self.settings.transcription_provider
        if provider == "whisper-server":
            try:
                async with httpx.AsyncClient(timeout=5) as client:
                    response = await client.get(
                        f"{self.settings.whisper_server_url}/v1/models"
                    )
                response.raise_for_status()
                model_ids = {
                    str(item.get("id", ""))
                    for item in response.json().get("data", [])
                }
                configured = self.settings.whisper_server_model
                if configured not in model_ids:
                    return {
                        "status": "error",
                        "provider": provider,
                        "detail": f"model unavailable: {configured}",
                    }
                return {
                    "status": "ok",
                    "provider": provider,
                    "detail": configured,
                }
            except Exception as error:
                return {
                    "status": "error",
                    "provider": provider,
                    "detail": str(error)[:160],
                }
        if provider == "openai":
            ready = bool(self.settings.openai_api_key)
            return {
                "status": "ok" if ready else "error",
                "provider": provider,
                "detail": "configured" if ready else "API key missing",
            }
        if provider in {"mock", "faster-whisper"}:
            return {"status": "ok", "provider": provider, "detail": "configured"}
        return {"status": "error", "provider": provider, "detail": "unsupported"}

    async def _openai(self, wav_path: Path, original_filename: str) -> str:
        if not self.settings.openai_api_key:
            raise RuntimeError("OPENAI_API_KEY is required for OpenAI transcription")
        async with httpx.AsyncClient(
            timeout=self.settings.transcription_timeout_seconds
        ) as client:
            with wav_path.open("rb") as audio:
                response = await client.post(
                    "https://api.openai.com/v1/audio/transcriptions",
                    headers={
                        "Authorization": f"Bearer {self.settings.openai_api_key}"
                    },
                    data={"model": self.settings.transcription_model},
                    files={"file": (original_filename, audio, "audio/wav")},
                )
        response.raise_for_status()
        text = self._response_text(response, "OpenAI transcription provider")
        if not text:
            raise RuntimeError("Transcription provider returned empty text")
        return text

    async def _whisper_server(
        self, wav_path: Path, original_filename: str
    ) -> str:
        base_url = self.settings.whisper_server_url
        parsed = urlparse(base_url)
        if parsed.scheme != "http" or parsed.hostname not in {
            "127.0.0.1",
            "localhost",
            "::1",
        }:
            raise RuntimeError(
                "WHISPER_SERVER_URL must be a loopback HTTP address"
            )
        headers = {}
        if self.settings.whisper_server_api_key:
            headers["Authorization"] = (
                f"Bearer {self.settings.whisper_server_api_key}"
            )
        async with httpx.AsyncClient(
            timeout=self.settings.transcription_timeout_seconds
        ) as client:
            texts = await self._whisper_wav_chunks(
                client, wav_path, original_filename, headers
            )
        text = "\n\n".join(part for part in texts if part).strip()
        if not text:
            raise RuntimeError("Local Whisper server returned empty text")
        return text

    async def _whisper_wav_chunks(
        self,
        client: httpx.AsyncClient,
        wav_path: Path,
        original_filename: str,
        headers: dict[str, str],
    ) -> list[str]:
        chunk_seconds = self.settings.whisper_server_chunk_seconds
        if not isinstance(chunk_seconds, int) or chunk_seconds <= 0:
            raise RuntimeError(
                "WHISPER_SERVER_CHUNK_SECONDS must be a positive integer"
            )
        with tempfile.TemporaryDirectory(prefix="cardputer-whisper-") as temp_dir:
            # Reading and re-writing a long WAV is blocking file I/O, so split it
            # in a worker thread instead of stalling the event loop.
            chunk_paths = await asyncio.to_thread(
                _split_wav, wav_path, Path(temp_dir), chunk_seconds
            )
            if not chunk_paths:
                return [
                    await self._request_whisper(
                        client, wav_path, original_filename, headers
                    )
                ]
            texts: list[str] = []
            for part_number, chunk_path in enumerate(chunk_paths, start=1):
                chunk_name = (
                    f"{Path(original_filename).stem}.part-{part_number:03d}.wav"
                )
                texts.append(
                    await self._request_whisper(
                        client, chunk_path, chunk_name, headers
                    )
                )
                chunk_path.unlink(missing_ok=True)
            return texts

    async def _request_whisper(
        self,
        client: httpx.AsyncClient,
        wav_path: Path,
        filename: str,
        headers: dict[str, str],
    ) -> str:
        request_data = {"model": self.settings.whisper_server_model}
        if self.settings.whisper_server_language:
            request_data["language"] = self.settings.whisper_server_language

        retries = self.settings.whisper_server_chunk_retries
        for attempt in range(retries + 1):
            try:
                with wav_path.open("rb") as audio:
                    response = await client.post(
                        f"{self.settings.whisper_server_url}/v1/audio/transcriptions",
                        headers=headers,
                        data=request_data,
                        files={"file": (filename, audio, "audio/wav")},
                    )
                response.raise_for_status()
                return self._response_text(response, "Whisper server")
            except httpx.HTTPStatusError as error:
                if error.response.status_code < 500 or attempt >= retries:
                    raise
            except httpx.TransportError:
                if attempt >= retries:
                    raise
            await asyncio.sleep(min(2**attempt, 5))
        raise RuntimeError("Whisper request retry loop ended unexpectedly")

    @staticmethod
    def _response_text(response: httpx.Response, provider: str) -> str:
        """Extract a usable transcript and turn malformed JSON into a clear error."""
        try:
            payload = response.json()
        except (ValueError, UnicodeError, TypeError) as error:
            raise RuntimeError(f"{provider} returned invalid JSON") from error
        if not isinstance(payload, dict):
            raise RuntimeError(f"{provider} returned an invalid response")
        text = payload.get("text", "")
        if not isinstance(text, str):
            raise RuntimeError(f"{provider} returned invalid transcript text")
        return text.strip()

    async def _faster_whisper(self, wav_path: Path) -> str:
        # Decoding is CPU-bound. Keep it away from FastAPI's event loop and
        # serialize requests so one compact Mac does not load several models.
        async with self._whisper_lock:
            return await asyncio.to_thread(self._transcribe_local, wav_path)

    def _transcribe_local(self, wav_path: Path) -> str:
        try:
            from faster_whisper import WhisperModel
        except ImportError as error:
            raise RuntimeError(
                "Install the gateway's local-whisper extra for faster-whisper"
            ) from error
        if self._whisper_model is None:
            self._whisper_model = WhisperModel(
                self.settings.whisper_model,
                device="cpu",
                compute_type="int8",
            )
        segments, _ = self._whisper_model.transcribe(
            str(wav_path),
            vad_filter=True,
            beam_size=5,
        )
        text = " ".join(segment.text.strip() for segment in segments).strip()
        if not text:
            raise RuntimeError("Local transcription returned empty text")
        return text


def _split_wav(wav_path: Path, temp_dir: Path, chunk_seconds: int) -> list[Path]:
    """Split a WAV into fixed-length parts. Returns [] when no split is needed."""
    with wave.open(str(wav_path), "rb") as source:
        frames_per_chunk = source.getframerate() * chunk_seconds
        if frames_per_chunk <= 0:
            raise RuntimeError("WAV sample rate must be positive")
        if source.getnframes() <= frames_per_chunk:
            return []
        parts: list[Path] = []
        while source.tell() < source.getnframes():
            frames = source.readframes(frames_per_chunk)
            if not frames:
                break
            chunk_path = temp_dir / f"part-{len(parts) + 1:03d}.wav"
            with wave.open(str(chunk_path), "wb") as chunk:
                chunk.setnchannels(source.getnchannels())
                chunk.setsampwidth(source.getsampwidth())
                chunk.setframerate(source.getframerate())
                chunk.setcomptype(source.getcomptype(), source.getcompname())
                chunk.writeframes(frames)
            parts.append(chunk_path)
        return parts
