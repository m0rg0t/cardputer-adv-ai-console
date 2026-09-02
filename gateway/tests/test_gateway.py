from __future__ import annotations

import asyncio
import io
import shlex
import sqlite3
import wave
from pathlib import Path

import httpx
import pytest
from fastapi import HTTPException

from voice_gateway.agent import AgentFormatter
from voice_gateway.app import _validate_wav, create_app
from voice_gateway.codex import CodexAppServer, CodexApproval, CodexJob
from voice_gateway.config import Settings
from voice_gateway.speech import SpeechSynthesizer
from voice_gateway.transcription import Transcriber


class FakeCodex:
    def __init__(self) -> None:
        self.jobs: dict[str, CodexJob] = {}
        self.messages: list[tuple[str, str]] = []
        self.canceled_jobs: list[str] = []

    async def list_threads(self, limit: int = 10) -> list[dict]:
        return [
            {
                "id": "thread_12345678",
                "name": "Agent Console",
                "preview": "Build the Cardputer client",
                "cwd": "/workspace",
                "updatedAt": 123,
                "status": {"type": "idle"},
            }
        ][:limit]

    async def read_thread(self, thread_id: str) -> dict:
        return {
            "id": thread_id,
            "name": "Agent Console",
            "turns": [
                {
                    "items": [
                        {
                            "type": "userMessage",
                            "content": [{"type": "text", "text": "Hello"}],
                        },
                        {"type": "agentMessage", "text": "Ready."},
                    ]
                }
            ],
        }

    async def start_thread(self, cwd: str = "") -> dict:
        return {"id": "thread_new12345", "name": "Untitled", "cwd": cwd}

    async def start_turn(self, thread_id: str, text: str) -> CodexJob:
        self.messages.append((thread_id, text))
        job = CodexJob(
            id=f"job{len(self.jobs) + 1}",
            thread_id=thread_id,
            turn_id=f"turn{len(self.jobs) + 1}",
            status="in_progress",
        )
        self.jobs[job.id] = job
        return job

    def get_job(self, job_id: str) -> CodexJob | None:
        return self.jobs.get(job_id)

    async def answer_approval(
        self, job_id: str, approval_id: str, decision: str
    ) -> CodexJob:
        job = self.jobs[job_id]
        job.approvals.pop(approval_id)
        return job

    async def cancel_job(self, job_id: str) -> CodexJob:
        self.canceled_jobs.append(job_id)
        job = self.jobs[job_id]
        job.status = "interrupted"
        return job

    async def close(self) -> None:
        return None


def wav_bytes() -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(b"\x00\x00" * 1600)
    return output.getvalue()


@pytest.mark.asyncio
async def test_whisper_server_splits_long_wav_in_order(
    settings: Settings, tmp_path: Path
) -> None:
    wav_path = tmp_path / "long.wav"
    with wave.open(str(wav_path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(100)
        wav.writeframes(b"\x00\x00" * 2500)

    configured = Settings(
        **{
            **settings.__dict__,
            "transcription_provider": "whisper-server",
            "whisper_server_chunk_seconds": 10,
        }
    )
    transcriber = Transcriber(configured)
    seen: list[tuple[str, int]] = []

    async def fake_request(client, path, filename, headers):
        del client, headers
        with wave.open(str(path), "rb") as chunk:
            seen.append((filename, chunk.getnframes()))
        return f"text {len(seen)}"

    transcriber._request_whisper = fake_request  # type: ignore[method-assign]
    text = await transcriber.transcribe(wav_path, "REC0001.WAV")

    assert text == "text 1\n\ntext 2\n\ntext 3"
    assert seen == [
        ("REC0001.part-001.wav", 1000),
        ("REC0001.part-002.wav", 1000),
        ("REC0001.part-003.wav", 500),
    ]


@pytest.mark.asyncio
async def test_whisper_server_rejects_non_positive_chunk_size(
    settings: Settings, tmp_path: Path
) -> None:
    wav_path = tmp_path / "short.wav"
    wav_path.write_bytes(wav_bytes())
    configured = Settings(
        **{
            **settings.__dict__,
            "transcription_provider": "whisper-server",
            "whisper_server_chunk_seconds": 0,
        }
    )

    with pytest.raises(RuntimeError, match="positive integer"):
        await Transcriber(configured).transcribe(wav_path, "REC0009.WAV")


@pytest.fixture
def settings(tmp_path: Path) -> Settings:
    return Settings(
        device_token="test-device-token-1234",
        vault_root=tmp_path / "vault",
        spool_root=tmp_path / "spool",
        database_path=tmp_path / "voice.sqlite3",
        transcription_provider="mock",
        audio_export_format="none",
    )


@pytest.mark.asyncio
async def test_upload_creates_one_note_and_deduplicates(settings: Settings) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": settings.device_token,
        "X-Voice-Filename": "REC0001.WAV",
        "X-Device-Name": "test-cardputer",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        first = await client.post("/v1/voice-notes", content=audio, headers=headers)
        second = await client.post("/v1/voice-notes", content=audio, headers=headers)
    assert first.status_code == 201
    assert second.status_code == 200
    assert second.json()["status"] == "duplicate"
    notes = list((settings.vault_root / settings.notes_folder).glob("*.md"))
    assert len(notes) == 1
    assert "Test transcription for REC0001.WAV" in notes[0].read_text()


@pytest.mark.asyncio
async def test_upload_exports_mp3_and_embeds_it_in_note(
    settings: Settings, tmp_path: Path
) -> None:
    fake_ffmpeg = tmp_path / "fake-ffmpeg"
    fake_ffmpeg.write_text(
        "#!/bin/sh\nfor output do :; done\nprintf 'ID3test' > \"$output\"\n",
        encoding="utf-8",
    )
    fake_ffmpeg.chmod(0o755)
    configured = Settings(
        **{
            **settings.__dict__,
            "audio_export_format": "mp3",
            "ffmpeg_executable": str(fake_ffmpeg),
        }
    )
    app = create_app(configured)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": configured.device_token,
        "X-Voice-Filename": "REC-AUDIO.WAV",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post("/v1/voice-notes", content=audio, headers=headers)

    assert response.status_code == 201
    note_path = Path(response.json()["note_path"])
    mp3_path = note_path.with_suffix(".mp3")
    assert mp3_path.read_bytes() == b"ID3test"
    assert f'audio_file: "{mp3_path.name}"' in note_path.read_text()
    assert f"![[{mp3_path.name}]]" in note_path.read_text()


@pytest.mark.asyncio
async def test_upload_reports_empty_mp3_export(
    settings: Settings, tmp_path: Path
) -> None:
    fake_ffmpeg = tmp_path / "fake-ffmpeg"
    fake_ffmpeg.write_text(
        "#!/bin/sh\nfor output do :; done\n: > \"$output\"\n",
        encoding="utf-8",
    )
    fake_ffmpeg.chmod(0o755)
    configured = Settings(
        **{
            **settings.__dict__,
            "audio_export_format": "mp3",
            "ffmpeg_executable": str(fake_ffmpeg),
        }
    )
    app = create_app(configured)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": configured.device_token,
        "X-Voice-Filename": "REC-EMPTY-MP3.WAV",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post("/v1/voice-notes", content=audio, headers=headers)

    assert response.status_code == 503
    assert "produced no audio" in response.json()["detail"]
    assert not list(configured.vault_root.rglob("*.mp3"))


@pytest.mark.asyncio
async def test_rejects_wrong_token(settings: Settings) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post(
            "/v1/voice-notes",
            content=audio,
            headers={
                "X-Device-Token": "wrong-token-xxxxxxxx",
                "Content-Length": str(len(audio)),
            },
        )
    assert response.status_code == 401


@pytest.mark.asyncio
async def test_rejects_malformed_content_length_without_server_error(
    settings: Settings,
) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post(
            "/v1/voice-notes",
            content=wav_bytes(),
            headers={
                "X-Device-Token": settings.device_token,
                "Content-Length": "not-a-number",
            },
        )
    assert response.status_code == 400
    assert response.json()["detail"] == "Invalid Content-Length"


def test_validate_wav_maps_os_errors_to_client_error(settings: Settings) -> None:
    missing = settings.spool_root / "missing.wav"
    with pytest.raises(HTTPException) as error:
        _validate_wav(missing)
    assert error.value.status_code == 415


def test_validate_wav_rejects_truncated_pcm_data(settings: Settings) -> None:
    truncated = settings.spool_root / "truncated.wav"
    truncated.parent.mkdir(parents=True, exist_ok=True)
    truncated.write_bytes(wav_bytes()[:-1])
    with pytest.raises(HTTPException) as error:
        _validate_wav(truncated)
    assert error.value.status_code == 415
    assert error.value.detail == "Truncated WAV audio"


@pytest.mark.asyncio
async def test_authenticated_status_reports_dependencies(settings: Settings) -> None:
    codex = FakeCodex()
    app = create_app(settings, codex)
    transport = httpx.ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.get(
            "/v1/status", headers={"X-Device-Token": settings.device_token}
        )
    assert response.status_code == 200
    assert response.json()["gateway"]["status"] == "ok"
    assert response.json()["whisper"]["status"] == "ok"
    assert response.json()["formatter"]["status"] == "disabled"
    assert response.json()["codex"]["status"] == "disabled"
    assert response.json()["speech"]["status"] == "disabled"


@pytest.mark.asyncio
async def test_retry_all_failed_voice_jobs_for_device(settings: Settings) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": settings.device_token,
        "X-Device-Name": "test-cardputer",
        "X-Voice-Filename": "FAILED.WAV",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        accepted = await client.post("/v1/voice/jobs", content=audio, headers=headers)
        job_id = accepted.json()["id"]
        app.state.store.update_voice_job(
            job_id, status="failed", stage="transcribing", error="connection timeout"
        )
        retried = await client.post("/v1/voice/jobs/retry-failed", headers=headers)
    assert retried.status_code == 202
    assert retried.json()["count"] == 1
    assert app.state.store.get_voice_job(job_id).status == "retrying"


@pytest.mark.asyncio
async def test_codex_speech_reports_disabled_tts(settings: Settings) -> None:
    app = create_app(settings, FakeCodex())
    transport = httpx.ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.get(
            "/v1/codex/chats/thread_12345678/speech",
            headers={"X-Device-Token": settings.device_token},
        )
    assert response.status_code == 503
    assert "TTS_ENABLED" in response.json()["detail"]


def test_speech_cleanup_makes_markdown_readable(settings: Settings) -> None:
    speech = SpeechSynthesizer(settings)
    assert (
        speech._clean("**Done** [report](https://example.com) `ok`") == "Done report ok"
    )


def test_formatter_rejects_malformed_chat_completion() -> None:
    response = httpx.Response(200, json={"choices": [{"message": {}}]})
    with pytest.raises(RuntimeError, match="empty text"):
        AgentFormatter._response_text(response, "Transcript formatter")


def test_speech_http_error_handles_non_object_json() -> None:
    response = httpx.Response(502, json=["upstream unavailable"])
    assert SpeechSynthesizer._http_error(response) == (
        "HTTP 502: ['upstream unavailable']"
    )


@pytest.mark.parametrize(
    "payload, message",
    [([], "invalid response"), ({"text": 42}, "invalid transcript text")],
)
def test_transcriber_rejects_malformed_provider_response(
    payload: object, message: str
) -> None:
    response = httpx.Response(200, json=payload)
    with pytest.raises(RuntimeError, match=message):
        Transcriber._response_text(response, "Whisper server")


@pytest.mark.asyncio
async def test_elevenlabs_speech_resolves_named_voice_and_caches_wav(
    settings: Settings, tmp_path: Path
) -> None:
    expected_wav = wav_bytes()
    fixture = tmp_path / "tts-output.wav"
    fixture.write_bytes(expected_wav)
    fake_ffmpeg = tmp_path / "fake-ffmpeg"
    fake_ffmpeg.write_text(
        "#!/bin/sh\nfor output do :; done\n"
        f"cp {shlex.quote(str(fixture))} \"$output\"\n",
        encoding="utf-8",
    )
    fake_ffmpeg.chmod(0o755)
    requests: list[str] = []

    def elevenlabs(request: httpx.Request) -> httpx.Response:
        requests.append(request.url.path)
        assert request.headers["xi-api-key"] == "test-elevenlabs-key"
        if request.url.path == "/v2/voices":
            assert request.url.params["search"] == "Jarvis"
            return httpx.Response(
                200,
                json={
                    "voices": [{"voice_id": "jarvisvoice123456789", "name": "Jarvis"}]
                },
            )
        assert request.url.path.endswith("/jarvisvoice123456789")
        return httpx.Response(200, content=b"ID3-elevenlabs-audio")

    configured = Settings(
        **{
            **settings.__dict__,
            "tts_enabled": True,
            "tts_provider": "elevenlabs",
            "elevenlabs_api_key": "test-elevenlabs-key",
            "elevenlabs_voice": "Jarvis",
            "ffmpeg_executable": str(fake_ffmpeg),
        }
    )
    speech = SpeechSynthesizer(configured, httpx.MockTransport(elevenlabs))
    first = await speech.synthesize("**Готово.**")
    second = await speech.synthesize("**Готово.**")

    assert first == expected_wav
    assert second == first
    assert requests == ["/v2/voices", "/v1/text-to-speech/jarvisvoice123456789"]


@pytest.mark.asyncio
async def test_concurrent_tts_requests_share_one_cache_fill(
    settings: Settings, tmp_path: Path
) -> None:
    expected_wav = wav_bytes()
    fixture = tmp_path / "tts-output.wav"
    fixture.write_bytes(expected_wav)
    fake_ffmpeg = tmp_path / "fake-ffmpeg"
    fake_ffmpeg.write_text(
        "#!/bin/sh\nfor output do :; done\n"
        f"cp {shlex.quote(str(fixture))} \"$output\"\n",
        encoding="utf-8",
    )
    fake_ffmpeg.chmod(0o755)
    requests: list[str] = []

    def elevenlabs(request: httpx.Request) -> httpx.Response:
        requests.append(request.url.path)
        if request.url.path == "/v2/voices":
            return httpx.Response(
                200,
                json={"voices": [{"voice_id": "jarvisvoice123456789", "name": "Jarvis"}]},
            )
        return httpx.Response(200, content=b"ID3-elevenlabs-audio")

    configured = Settings(
        **{
            **settings.__dict__,
            "tts_enabled": True,
            "tts_provider": "elevenlabs",
            "elevenlabs_api_key": "test-elevenlabs-key",
            "elevenlabs_voice": "Jarvis",
            "ffmpeg_executable": str(fake_ffmpeg),
        }
    )
    speech = SpeechSynthesizer(configured, httpx.MockTransport(elevenlabs))
    first, second = await asyncio.gather(
        speech.synthesize("same response"), speech.synthesize("same response")
    )

    assert first == second == expected_wav
    assert requests == ["/v2/voices", "/v1/text-to-speech/jarvisvoice123456789"]


@pytest.mark.asyncio
async def test_tts_rejects_invalid_converter_output(
    settings: Settings, tmp_path: Path
) -> None:
    fake_ffmpeg = tmp_path / "fake-ffmpeg"
    fake_ffmpeg.write_text(
        "#!/bin/sh\nfor output do :; done\nprintf 'not-a-wav' > \"$output\"\n",
        encoding="utf-8",
    )
    fake_ffmpeg.chmod(0o755)

    def elevenlabs(request: httpx.Request) -> httpx.Response:
        if request.url.path == "/v2/voices":
            return httpx.Response(
                200,
                json={"voices": [{"voice_id": "jarvisvoice123456789", "name": "Jarvis"}]},
            )
        return httpx.Response(200, content=b"ID3-elevenlabs-audio")

    configured = Settings(
        **{
            **settings.__dict__,
            "tts_enabled": True,
            "tts_provider": "elevenlabs",
            "elevenlabs_api_key": "test-elevenlabs-key",
            "elevenlabs_voice": "Jarvis",
            "ffmpeg_executable": str(fake_ffmpeg),
        }
    )
    speech = SpeechSynthesizer(configured, httpx.MockTransport(elevenlabs))

    with pytest.raises(RuntimeError, match="invalid WAV"):
        await speech.synthesize("broken output")
    assert not list((configured.spool_root / "tts-cache").glob("*.wav"))


def test_tts_wav_validation_rejects_truncated_pcm_data() -> None:
    assert not SpeechSynthesizer._is_valid_wav(wav_bytes()[:-1])


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "payload",
    [
        [],
        {"voices": {"voice_id": "not-a-list"}},
        {"voices": ["not-a-voice"]},
    ],
)
async def test_tts_rejects_malformed_voice_lookup_payload(
    settings: Settings, payload: object
) -> None:
    def elevenlabs(request: httpx.Request) -> httpx.Response:
        assert request.url.path == "/v2/voices"
        return httpx.Response(200, json=payload)

    configured = Settings(
        **{
            **settings.__dict__,
            "tts_enabled": True,
            "tts_provider": "elevenlabs",
            "elevenlabs_api_key": "test-elevenlabs-key",
            "elevenlabs_voice": "Jarvis",
        }
    )
    speech = SpeechSynthesizer(configured, httpx.MockTransport(elevenlabs))

    with pytest.raises(RuntimeError, match="invalid voice data"):
        await speech._elevenlabs_voice_id()


def test_title_fallback_is_readable_and_bounded(settings: Settings) -> None:
    formatter = AgentFormatter(settings)
    title = formatter._fallback_title(
        "Так, пожалуйста, запиши обсудить перенос длинных названий чатов. "
        "Остальные детали позже."
    )
    assert title == "обсудить перенос длинных названий чатов"
    assert len(title) <= 72


@pytest.mark.asyncio
async def test_codex_exec_formatter_uses_stdin_and_returns_final_text(
    settings: Settings, tmp_path: Path
) -> None:
    executable = tmp_path / "fake-codex"
    executable.write_text(
        "#!/bin/sh\ncat >/dev/null\nprintf 'Исправленный текст.'\n",
        encoding="utf-8",
    )
    executable.chmod(0o755)
    configured = Settings(
        **{
            **settings.__dict__,
            "transcript_formatter": "codex-exec",
            "codex_executable": str(executable),
            "codex_default_cwd": str(tmp_path),
        }
    )
    formatted = await AgentFormatter(configured).format("исходный тект")
    assert formatted == "Исправленный текст."


@pytest.mark.asyncio
async def test_codex_chat_list_read_and_message(settings: Settings) -> None:
    codex = FakeCodex()
    app = create_app(settings, codex)
    transport = httpx.ASGITransport(app=app)
    headers = {"X-Device-Token": settings.device_token}
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        chats = await client.get("/v1/codex/chats", headers=headers)
        chat = await client.get("/v1/codex/chats/thread_12345678", headers=headers)
        sent = await client.post(
            "/v1/codex/chats/thread_12345678/messages",
            headers=headers,
            json={"text": "Run tests"},
        )
    assert chats.status_code == 200
    assert chats.json()["data"][0]["name"] == "Agent Console"
    assert chat.json()["messages"][-1] == {"role": "assistant", "text": "Ready."}
    assert sent.status_code == 202
    assert sent.json()["id"] == "job1"
    assert codex.messages == [("thread_12345678", "Run tests")]


@pytest.mark.asyncio
async def test_codex_rpc_reader_survives_malformed_lines_and_errors() -> None:
    class FakeStdout:
        def __init__(self) -> None:
            self.lines = [
                b"[]\n",
                b'{"id": 1, "error": ["bad response"]}\n',
            ]

        async def readline(self) -> bytes:
            return self.lines.pop(0) if self.lines else b""

    class FakeProcess:
        stdout = FakeStdout()

    server = CodexAppServer()
    server.process = FakeProcess()  # type: ignore[assignment]
    future = asyncio.get_running_loop().create_future()
    server.pending[1] = future

    await server._read_stdout()

    with pytest.raises(RuntimeError, match="invalid error"):
        await future


@pytest.mark.asyncio
async def test_codex_shutdown_fails_pending_jobs_and_requests() -> None:
    server = CodexAppServer()
    future = asyncio.get_running_loop().create_future()
    server.pending[1] = future
    job = CodexJob(
        id="job-running",
        thread_id="thread_12345678",
        turn_id="turn-1",
        status="in_progress",
    )
    server.jobs[job.id] = job

    await server._shutdown_process("Codex App Server stopped")

    with pytest.raises(RuntimeError, match="stopped"):
        await future
    assert job.status == "failed"
    assert job.error == "Codex App Server stopped"
    assert not server.pending


@pytest.mark.asyncio
async def test_codex_cancel_before_turn_start_is_local_and_idempotent() -> None:
    server = CodexAppServer()
    job = CodexJob(
        id="job-starting",
        thread_id="thread_12345678",
        status="starting",
    )
    server.jobs[job.id] = job
    server.starting_thread_jobs[job.thread_id] = job.id

    canceled = await server.cancel_job(job.id)
    repeated = await server.cancel_job(job.id)

    assert canceled is repeated is job
    assert job.status == "interrupted"
    assert job.error == "Canceled before turn started"
    assert not server.starting_thread_jobs


@pytest.mark.asyncio
async def test_codex_endpoints_return_503_for_malformed_thread_payload(
    settings: Settings,
) -> None:
    class MalformedCodex(FakeCodex):
        async def list_threads(self, limit: int = 10) -> list[dict]:
            del limit
            return ["not-a-thread"]  # type: ignore[list-item]

        async def read_thread(self, thread_id: str) -> dict:
            return {"id": thread_id, "turns": "not-a-list"}

    app = create_app(settings, MalformedCodex())
    transport = httpx.ASGITransport(app=app)
    headers = {"X-Device-Token": settings.device_token}
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        chats = await client.get("/v1/codex/chats", headers=headers)
        chat = await client.get(
            "/v1/codex/chats/thread_12345678", headers=headers
        )
        speech = await client.get(
            "/v1/codex/chats/thread_12345678/speech", headers=headers
        )

    assert chats.status_code == 503
    assert "invalid thread" in chats.json()["detail"]
    assert chat.status_code == 503
    assert "invalid thread turns" in chat.json()["detail"]
    assert speech.status_code == 503
    assert "invalid thread turns" in speech.json()["detail"]


@pytest.mark.asyncio
async def test_routed_voice_both_creates_note_and_codex_job(settings: Settings) -> None:
    codex = FakeCodex()
    app = create_app(settings, codex)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": settings.device_token,
        "X-Voice-Filename": "REC0002.WAV",
        "X-Device-Name": "test-cardputer",
        "X-Voice-Destination": "both",
        "X-Codex-Thread-ID": "thread_12345678",
        "X-Compact-Response": "true",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        first = await client.post("/v1/voice", content=audio, headers=headers)
        duplicate = await client.post("/v1/voice", content=audio, headers=headers)
    assert first.status_code == 201
    assert first.json()["destination"] == "both"
    assert first.json()["job_id"] == "job1"
    assert "transcript" not in first.json()
    assert Path(first.json()["note_path"]).exists()
    assert duplicate.status_code == 200
    assert duplicate.json()["status"] == "duplicate"
    assert len(codex.messages) == 1


@pytest.mark.asyncio
async def test_routed_voice_returns_bounded_transcript_preview(
    settings: Settings,
) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": settings.device_token,
        "X-Voice-Filename": "REC0003.WAV",
        "X-Voice-Destination": "note",
        "X-Compact-Response": "false",
        "X-Transcript-Max-Chars": "10",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        created = await client.post("/v1/voice", content=audio, headers=headers)
        duplicate = await client.post("/v1/voice", content=audio, headers=headers)

    assert created.status_code == 201
    assert created.json()["transcript"] == "Test trans"
    assert created.json()["transcript_complete"] is False
    assert duplicate.status_code == 200
    assert duplicate.json()["transcript"] == "Test trans"
    assert duplicate.json()["transcript_complete"] is False


@pytest.mark.asyncio
async def test_async_voice_job_reports_progress_and_completes(
    settings: Settings,
) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": settings.device_token,
        "X-Voice-Filename": "REC0004.WAV",
        "X-Device-Name": "test-cardputer",
        "X-Voice-Destination": "note",
        "X-Voice-Profile": "meeting",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        accepted = await client.post("/v1/voice/jobs", content=audio, headers=headers)
        assert accepted.status_code == 202
        assert accepted.json()["status"] == "accepted"
        job_id = accepted.json()["id"]

        processed = await app.state.process_pending_voice_jobs()
        completed = await client.get(f"/v1/voice/jobs/{job_id}", headers=headers)
        duplicate = await client.post("/v1/voice/jobs", content=audio, headers=headers)

    assert processed == 1
    assert completed.status_code == 200
    assert completed.json()["status"] == "completed"
    assert completed.json()["progress"] == 100
    assert completed.json()["profile"] == "meeting"
    assert completed.json()["title"] == "Test transcription for REC0004.WAV"
    assert completed.json()["suggested_filename"].endswith(
        " - Test transcription for REC0004.WAV"
    )
    assert completed.json()["transcript"] == "Test transcription for REC0004.WAV."
    note_path = Path(completed.json()["note_path"])
    assert note_path.exists()
    assert "Test transcription for REC0004.WAV" in note_path.name
    assert "# Test transcription for REC0004.WAV" in note_path.read_text()
    assert duplicate.status_code == 200
    assert duplicate.json()["id"] == job_id


@pytest.mark.asyncio
async def test_async_voice_job_can_be_canceled_before_processing(
    settings: Settings,
) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": settings.device_token,
        "X-Voice-Filename": "REC0005.WAV",
        "X-Voice-Destination": "note",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        accepted = await client.post("/v1/voice/jobs", content=audio, headers=headers)
        job_id = accepted.json()["id"]
        canceled = await client.post(f"/v1/voice/jobs/{job_id}/cancel", headers=headers)
        processed = await app.state.process_pending_voice_jobs()

    assert canceled.status_code == 200
    assert canceled.json()["status"] == "canceled"
    assert processed == 0


def test_voice_job_completion_cannot_resurrect_cancellation(settings: Settings) -> None:
    app = create_app(settings)
    store = app.state.store
    audio_path = settings.spool_root / "race.wav"
    job = store.create_voice_job(
        job_id="race-job",
        route_key="race-route",
        digest="a" * 64,
        original_filename="RACE.WAV",
        device_name="test-cardputer",
        destination="note",
        thread_id="",
        profile="default",
        audio_path=audio_path,
    )
    store.update_voice_job(
        job.id, status="canceled", stage="canceled", error="Canceled by device"
    )

    result = store.update_voice_job_unless_canceled(
        job.id,
        status="completed",
        stage="completed",
        progress=100,
        error="",
    )

    assert result.status == "canceled"
    assert result.stage == "canceled"


@pytest.mark.asyncio
async def test_async_voice_job_retries_are_idempotent_on_insert_race(
    settings: Settings, monkeypatch: pytest.MonkeyPatch
) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": settings.device_token,
        "X-Voice-Filename": "REC0008.WAV",
        "X-Voice-Destination": "note",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    store = app.state.store
    real_find = store.find_voice_job_by_route
    real_create = store.create_voice_job
    reads = 0

    def hide_first_read(route_key: str):
        nonlocal reads
        reads += 1
        return None if reads == 1 else real_find(route_key)

    def create_winner_then_report_conflict(**kwargs):
        winner_audio = settings.spool_root / "winner.wav"
        winner_audio.write_bytes(audio)
        winner_kwargs = {**kwargs, "job_id": "winner-job", "audio_path": winner_audio}
        real_create(**winner_kwargs)
        raise sqlite3.IntegrityError("UNIQUE constraint failed: voice_jobs.route_key")

    monkeypatch.setattr(store, "find_voice_job_by_route", hide_first_read)
    monkeypatch.setattr(store, "create_voice_job", create_winner_then_report_conflict)

    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post("/v1/voice/jobs", content=audio, headers=headers)

    assert response.status_code == 202
    assert response.json()["id"] == "winner-job"
    assert (settings.spool_root / "winner.wav").exists()
    assert len(list(settings.spool_root.glob("*.wav"))) == 1


@pytest.mark.asyncio
async def test_async_voice_job_cancels_remote_codex_turn_after_race(
    settings: Settings,
) -> None:
    class BlockingCodex(FakeCodex):
        def __init__(self) -> None:
            super().__init__()
            self.turn_started = asyncio.Event()
            self.release_turn = asyncio.Event()

        async def start_turn(self, thread_id: str, text: str) -> CodexJob:
            self.turn_started.set()
            await self.release_turn.wait()
            return await super().start_turn(thread_id, text)

    codex = BlockingCodex()
    app = create_app(settings, codex)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": settings.device_token,
        "X-Voice-Filename": "REC0007.WAV",
        "X-Voice-Destination": "codex",
        "X-Codex-Thread-ID": "thread_12345678",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        accepted = await client.post("/v1/voice/jobs", content=audio, headers=headers)
        job_id = accepted.json()["id"]
        processing = asyncio.create_task(app.state.process_pending_voice_jobs())
        await codex.turn_started.wait()
        canceled = await client.post(
            f"/v1/voice/jobs/{job_id}/cancel", headers=headers
        )
        codex.release_turn.set()
        await processing
        result = await client.get(f"/v1/voice/jobs/{job_id}", headers=headers)

    assert canceled.status_code == 200
    assert result.json()["status"] == "canceled"
    assert codex.canceled_jobs == ["job1"]


@pytest.mark.asyncio
async def test_reprocess_reuses_transcript_with_another_profile(
    settings: Settings,
) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    headers = {
        "X-Device-Token": settings.device_token,
        "X-Voice-Filename": "REC0006.WAV",
        "X-Voice-Destination": "note",
        "Content-Type": "audio/wav",
        "Content-Length": str(len(audio)),
    }
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        accepted = await client.post("/v1/voice/jobs", content=audio, headers=headers)
        await app.state.process_pending_voice_jobs()
        source_id = accepted.json()["id"]
        reprocess = await client.post(
            f"/v1/voice/jobs/{source_id}/reprocess",
            headers={"X-Device-Token": settings.device_token},
            json={"profile": "idea"},
        )
        await app.state.process_pending_voice_jobs()
        result = await client.get(
            f"/v1/voice/jobs/{reprocess.json()['id']}",
            headers={"X-Device-Token": settings.device_token},
        )

    assert reprocess.status_code == 202
    assert result.json()["status"] == "completed"
    assert result.json()["profile"] == "idea"
    assert result.json()["transcript"] == "Test transcription for REC0006.WAV."
    assert "/Ideas/" in result.json()["note_path"]


@pytest.mark.asyncio
async def test_codex_approval_response(settings: Settings) -> None:
    codex = FakeCodex()
    job = await codex.start_turn("thread_12345678", "Change a file")
    job.approvals["approval1"] = CodexApproval(
        id="approval1",
        rpc_id=5,
        method="item/fileChange/requestApproval",
        thread_id=job.thread_id,
        turn_id=job.turn_id,
        reason="Write config",
    )
    app = create_app(settings, codex)
    transport = httpx.ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post(
            f"/v1/codex/jobs/{job.id}/approvals/approval1",
            headers={"X-Device-Token": settings.device_token},
            json={"decision": "decline"},
        )
    assert response.status_code == 200
    assert response.json()["approvals"] == []


@pytest.mark.asyncio
async def test_rejects_unknown_voice_profile(settings: Settings) -> None:
    app = create_app(settings)
    transport = httpx.ASGITransport(app=app)
    audio = wav_bytes()
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post(
            "/v1/voice/jobs",
            content=audio,
            headers={
                "X-Device-Token": settings.device_token,
                "X-Voice-Profile": "unknown",
                "Content-Length": str(len(audio)),
            },
        )
    assert response.status_code == 400
    assert response.json()["detail"] == "Invalid voice profile"


def test_codex_prunes_only_finished_jobs() -> None:
    server = CodexAppServer()
    for index in range(105):
        job = CodexJob(
            id=f"done-{index}",
            thread_id="thread_12345678",
            turn_id=f"turn-{index}",
            status="completed",
        )
        server.jobs[job.id] = job
        server.turn_jobs[job.turn_id] = job.id
    active = CodexJob(id="active", thread_id="thread_12345678", status="in_progress")
    server.jobs[active.id] = active

    server._prune_finished_jobs(keep=100)

    assert "active" in server.jobs
    assert len(server.jobs) == 101
    assert "done-0" not in server.jobs and "turn-0" not in server.turn_jobs
    assert "done-104" in server.jobs
