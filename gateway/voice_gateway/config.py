from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    device_token: str
    vault_root: Path
    notes_folder: str = "Inbox/Cardputer Voice"
    spool_root: Path = Path("./data/spool")
    database_path: Path = Path("./data/voice-notes.sqlite3")
    max_upload_bytes: int = 32 * 1024 * 1024
    transcription_provider: str = "mock"
    openai_api_key: str = ""
    transcription_model: str = "gpt-4o-mini-transcribe"
    whisper_model: str = "small"
    whisper_server_url: str = "http://127.0.0.1:12017"
    whisper_server_model: str = "medium-q5_0"
    whisper_server_language: str = "ru"
    whisper_server_api_key: str = ""
    transcription_timeout_seconds: int = 1800
    agent_base_url: str = ""
    agent_api_key: str = ""
    agent_model: str = ""
    transcript_formatter: str = "none"
    audio_export_format: str = "mp3"
    ffmpeg_executable: str = "ffmpeg"
    tts_enabled: bool = False
    tts_voice: str = "Milena"
    tts_rate: int = 190
    tts_max_chars: int = 6000
    say_executable: str = "say"
    codex_formatter_timeout_seconds: int = 180
    codex_enabled: bool = False
    codex_executable: str = "codex"
    codex_default_cwd: str = ""
    advertise_mdns: bool = True
    advertise_port: int = 8765

    @classmethod
    def from_env(cls) -> "Settings":
        token = os.environ.get("VOICE_DEVICE_TOKEN", "")
        if len(token) < 16:
            raise RuntimeError("VOICE_DEVICE_TOKEN must contain at least 16 characters")
        vault = os.environ.get("OBSIDIAN_VAULT_ROOT", "")
        if not vault:
            raise RuntimeError("OBSIDIAN_VAULT_ROOT is required")
        return cls(
            device_token=token,
            vault_root=Path(vault).expanduser(),
            notes_folder=os.environ.get(
                "OBSIDIAN_NOTES_FOLDER", "Inbox/Cardputer Voice"
            ),
            spool_root=Path(os.environ.get("VOICE_SPOOL_ROOT", "./data/spool")),
            database_path=Path(
                os.environ.get("VOICE_DATABASE_PATH", "./data/voice-notes.sqlite3")
            ),
            max_upload_bytes=int(
                os.environ.get("VOICE_MAX_UPLOAD_BYTES", str(32 * 1024 * 1024))
            ),
            transcription_provider=os.environ.get(
                "TRANSCRIPTION_PROVIDER", "mock"
            ).lower(),
            openai_api_key=os.environ.get("OPENAI_API_KEY", ""),
            transcription_model=os.environ.get(
                "TRANSCRIPTION_MODEL", "gpt-4o-mini-transcribe"
            ),
            whisper_model=os.environ.get("WHISPER_MODEL", "small"),
            whisper_server_url=os.environ.get(
                "WHISPER_SERVER_URL", "http://127.0.0.1:12017"
            ).rstrip("/"),
            whisper_server_model=os.environ.get(
                "WHISPER_SERVER_MODEL", "medium-q5_0"
            ),
            whisper_server_language=os.environ.get(
                "WHISPER_SERVER_LANGUAGE", "ru"
            ),
            whisper_server_api_key=os.environ.get(
                "WHISPER_SERVER_API_KEY", ""
            ),
            transcription_timeout_seconds=int(
                os.environ.get("TRANSCRIPTION_TIMEOUT_SECONDS", "1800")
            ),
            agent_base_url=os.environ.get("AGENT_BASE_URL", "").rstrip("/"),
            agent_api_key=os.environ.get("AGENT_API_KEY", ""),
            agent_model=os.environ.get("AGENT_MODEL", ""),
            transcript_formatter=os.environ.get(
                "TRANSCRIPT_FORMATTER",
                "openai-compatible" if os.environ.get("AGENT_BASE_URL") else "none",
            ).lower(),
            audio_export_format=os.environ.get(
                "OBSIDIAN_AUDIO_FORMAT", "mp3"
            ).strip().lower(),
            ffmpeg_executable=os.environ.get(
                "FFMPEG_EXECUTABLE", "ffmpeg"
            ).strip(),
            tts_enabled=os.environ.get("TTS_ENABLED", "false").lower()
            in {"1", "true", "yes", "on"},
            tts_voice=os.environ.get("TTS_VOICE", "Milena").strip(),
            tts_rate=int(os.environ.get("TTS_RATE", "190")),
            tts_max_chars=int(os.environ.get("TTS_MAX_CHARS", "6000")),
            say_executable=os.environ.get("SAY_EXECUTABLE", "say").strip(),
            codex_formatter_timeout_seconds=int(
                os.environ.get("CODEX_FORMATTER_TIMEOUT_SECONDS", "180")
            ),
            codex_enabled=os.environ.get("CODEX_ENABLED", "false").lower()
            in {"1", "true", "yes", "on"},
            codex_executable=os.environ.get("CODEX_EXECUTABLE", "codex"),
            codex_default_cwd=os.environ.get("CODEX_DEFAULT_CWD", ""),
            advertise_mdns=os.environ.get("GATEWAY_ADVERTISE_MDNS", "true").lower()
            in {"1", "true", "yes", "on"},
            advertise_port=int(os.environ.get("GATEWAY_ADVERTISE_PORT", "8765")),
        )

    @property
    def notes_root(self) -> Path:
        vault = self.vault_root.resolve()
        target = (vault / self.notes_folder).resolve()
        if target != vault and vault not in target.parents:
            raise RuntimeError("OBSIDIAN_NOTES_FOLDER escapes the vault")
        return target
