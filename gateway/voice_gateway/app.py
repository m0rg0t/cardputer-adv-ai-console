from __future__ import annotations

import asyncio
import hashlib
import hmac
import os
import re
import sqlite3
import shutil
import tempfile
import uuid
import wave
from contextlib import asynccontextmanager
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import httpx
from fastapi import FastAPI, Header, HTTPException, Request, status
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel, Field

from .agent import AgentFormatter
from .codex import CodexAppServer, CodexBackend, CodexDisabled
from .config import Settings
from .profiles import PROFILES, get_profile
from .speech import SpeechSynthesizer
from .store import NoteStore
from .transcription import Transcriber

SAFE_FILENAME = re.compile(r"[^A-Za-z0-9._ -]+")
UNSAFE_TITLE_FILENAME = re.compile(r"[\\/:*?\"<>|\x00-\x1f]+")
SAFE_THREAD_ID = re.compile(r"^[A-Za-z0-9_-]{8,128}$")
SAFE_PROFILE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
VOICE_DESTINATIONS = {"note", "codex", "both"}
VOICE_PROCESSING_ERRORS = (
    RuntimeError,
    OSError,
    ValueError,
    EOFError,
    TimeoutError,
    wave.Error,
    httpx.HTTPError,
)


class CodexMessage(BaseModel):
    text: str = Field(min_length=1, max_length=12000)


class CodexApprovalDecision(BaseModel):
    decision: str


class NewCodexThread(BaseModel):
    cwd: str = ""


class ReprocessVoiceJob(BaseModel):
    profile: str = "default"
    destination: str = ""
    thread_id: str = ""


def _safe_name(value: str, fallback: str) -> str:
    value = Path(value).name
    cleaned = SAFE_FILENAME.sub("_", value).strip(" ._")
    return cleaned[:96] or fallback


def _yaml(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _safe_title(value: str) -> str:
    value = UNSAFE_TITLE_FILENAME.sub(" - ", value)
    value = re.sub(r"\s+", " ", value).strip(" .-_—–")
    return value[:72].strip() or "Голосовая заметка"


def _suggested_recording_filename(title: str, now: datetime | None = None) -> str:
    now = now or datetime.now().astimezone()
    recording_title = _safe_title(title)[:48]
    if recording_title.lower().endswith(".wav"):
        recording_title = recording_title[:-4].rstrip(" .-_")
    return f"{now:%Y-%m-%d %H-%M-%S} - {recording_title}.WAV"


def _validate_wav(path: Path) -> dict[str, int]:
    try:
        with wave.open(str(path), "rb") as wav:
            metadata = {
                "channels": wav.getnchannels(),
                "sample_rate": wav.getframerate(),
                "sample_width": wav.getsampwidth(),
                "frames": wav.getnframes(),
            }
            if metadata["channels"] <= 0 or metadata["sample_width"] <= 0:
                raise HTTPException(
                    status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, "Invalid WAV audio"
                )
            bytes_per_frame = metadata["channels"] * metadata["sample_width"]
            expected_audio_bytes = metadata["frames"] * bytes_per_frame
            read_audio_bytes = 0
            remaining_frames = metadata["frames"]
            while remaining_frames > 0:
                chunk = wav.readframes(min(remaining_frames, 4096))
                if not chunk or len(chunk) % bytes_per_frame != 0:
                    break
                read_audio_bytes += len(chunk)
                frames_read = len(chunk) // bytes_per_frame
                if frames_read == 0:
                    break
                remaining_frames -= frames_read
    except (wave.Error, EOFError, OSError, ValueError) as error:
        raise HTTPException(
            status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, "Invalid WAV"
        ) from error
    if metadata["channels"] != 1 or metadata["sample_width"] != 2:
        raise HTTPException(
            status.HTTP_415_UNSUPPORTED_MEDIA_TYPE,
            "Expected mono 16-bit PCM WAV",
        )
    if not 8000 <= metadata["sample_rate"] <= 48000 or metadata["frames"] == 0:
        raise HTTPException(status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, "Invalid WAV audio")
    if remaining_frames != 0 or read_audio_bytes != expected_audio_bytes:
        raise HTTPException(status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, "Truncated WAV audio")
    return metadata


def _content_length(request: Request) -> int:
    """Read the required length header without turning bad input into a 500."""
    raw = request.headers.get("content-length", "")
    try:
        return int(raw or 0)
    except (TypeError, ValueError) as error:
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST, "Invalid Content-Length"
        ) from error


def _voice_processing_http_error(error: Exception) -> HTTPException:
    detail = str(error).strip() or error.__class__.__name__
    return HTTPException(
        status.HTTP_503_SERVICE_UNAVAILABLE,
        f"Voice processing unavailable: {detail[:300]}",
    )


def _require_token(provided: str, expected: str) -> None:
    if not hmac.compare_digest(provided, expected):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Invalid device token")


def _thread_summary(thread: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(thread, dict):
        raise RuntimeError("Codex returned an invalid thread")
    thread_id = thread.get("id")
    if not isinstance(thread_id, str):
        thread_id = ""
    preview = thread.get("preview")
    if not isinstance(preview, str):
        preview = ""
    name = thread.get("name")
    if not isinstance(name, str) or not name.strip():
        name = preview or "Untitled"
    status_payload = thread.get("status")
    status_name = (
        status_payload.get("type") if isinstance(status_payload, dict) else "unknown"
    )
    if not isinstance(status_name, str) or not status_name:
        status_name = "unknown"
    return {
        "id": thread_id,
        "name": name,
        "preview": preview[:240],
        "cwd": thread.get("cwd", "")
        if isinstance(thread.get("cwd", ""), str)
        else "",
        "updated_at": thread.get("updatedAt"),
        "status": status_name,
    }


def _thread_messages(thread: dict[str, Any], limit: int = 16) -> list[dict[str, str]]:
    if not isinstance(thread, dict):
        raise RuntimeError("Codex returned an invalid thread")
    limit = max(1, min(limit, 64))
    turns = thread.get("turns") or []
    if not isinstance(turns, list):
        raise RuntimeError("Codex returned invalid thread turns")
    messages: list[dict[str, str]] = []
    for turn in turns:
        if not isinstance(turn, dict):
            continue
        items = turn.get("items") or []
        if not isinstance(items, list):
            continue
        for item in items:
            if not isinstance(item, dict):
                continue
            item_type = item.get("type")
            if item_type == "agentMessage":
                text = item.get("text", "")
                role = "assistant"
                if not isinstance(text, str):
                    continue
            elif item_type == "userMessage":
                pieces = item.get("content") or []
                if not isinstance(pieces, list):
                    continue
                text = "\n".join(
                    piece.get("text", "")
                    for piece in pieces
                    if isinstance(piece, dict)
                    and piece.get("type") == "text"
                    and isinstance(piece.get("text", ""), str)
                )
                role = "user"
            else:
                continue
            if text.strip():
                messages.append({"role": role, "text": text.strip()})
    return messages[-limit:]


def _write_note(
    settings: Settings,
    *,
    digest: str,
    original_filename: str,
    device_name: str,
    transcript: str,
    formatted: str | None,
    title: str,
    wav: dict[str, int],
    profile_id: str = "default",
    note_path: Path | None = None,
    audio_filename: str = "",
) -> Path:
    now = datetime.now(UTC)
    profile = get_profile(profile_id)
    title = _safe_title(title)
    root = (
        settings.notes_root / profile.folder if profile.folder else settings.notes_root
    )
    root.mkdir(parents=True, exist_ok=True)
    note_path = note_path or (
        root / f"{now:%Y-%m-%d %H-%M-%S} - {title} - {digest[:8]}.md"
    )
    body = [
        "---",
        "type: voice-note",
        f"created: {now.isoformat()}",
        f"device: {_yaml(device_name)}",
        f"source_file: {_yaml(original_filename)}",
        f"audio_sha256: {_yaml(digest)}",
        f"audio_file: {_yaml(audio_filename)}",
        f"profile: {_yaml(profile.id)}",
        f"title: {_yaml(title)}",
        f"sample_rate: {wav['sample_rate']}",
        "tags:",
        *[f"  - {tag}" for tag in profile.tags],
        "  - cardputer",
        "---",
        "",
        f"# {title}",
        "",
    ]
    if audio_filename:
        body.extend(["## Audio", "", f"![[{audio_filename}]]", ""])
    if formatted:
        body.extend([formatted, "", "## Raw transcript", "", transcript, ""])
    else:
        body.extend([transcript, ""])
    # A request can finish alongside another request (or a restarted worker)
    # for the same digest.  Never share a fixed ``.md.tmp`` path: one writer
    # could otherwise replace the other's partial note.
    temporary = note_path.with_name(f".{note_path.name}.{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_text("\n".join(body), encoding="utf-8")
        os.replace(temporary, note_path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    return note_path


def _new_note_path(
    settings: Settings,
    *,
    digest: str,
    original_filename: str,
    profile_id: str,
    title: str,
) -> Path:
    now = datetime.now().astimezone()
    profile = get_profile(profile_id)
    root = (
        settings.notes_root / profile.folder if profile.folder else settings.notes_root
    )
    root.mkdir(parents=True, exist_ok=True)
    return root / (f"{now:%Y-%m-%d %H-%M-%S} - {_safe_title(title)} - {digest[:8]}.md")


async def _export_note_audio(
    settings: Settings, wav_path: Path, note_path: Path
) -> Path | None:
    if settings.audio_export_format in {"", "none", "disabled"}:
        return None
    if settings.audio_export_format != "mp3":
        raise RuntimeError(
            f"Unsupported Obsidian audio format: {settings.audio_export_format}"
        )
    executable = shutil.which(settings.ffmpeg_executable)
    if not executable:
        raise RuntimeError(f"FFmpeg executable not found: {settings.ffmpeg_executable}")
    audio_path = note_path.with_suffix(".mp3")
    temporary = audio_path.with_name(f".{audio_path.name}.{uuid.uuid4().hex}.tmp")
    temporary.unlink(missing_ok=True)
    process = await asyncio.create_subprocess_exec(
        executable,
        "-nostdin",
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(wav_path),
        "-vn",
        "-codec:a",
        "libmp3lame",
        "-b:a",
        "64k",
        "-f",
        "mp3",
        str(temporary),
        stdout=asyncio.subprocess.DEVNULL,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        _, stderr = await asyncio.wait_for(process.communicate(), timeout=120)
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
        temporary.unlink(missing_ok=True)
        raise RuntimeError("MP3 conversion timed out") from error
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
        temporary.unlink(missing_ok=True)
        raise
    if process.returncode != 0:
        temporary.unlink(missing_ok=True)
        detail = stderr.decode("utf-8", "replace").strip()[-400:]
        raise RuntimeError(f"MP3 conversion failed: {detail or process.returncode}")
    try:
        if not temporary.is_file() or temporary.stat().st_size == 0:
            raise RuntimeError("MP3 conversion produced no audio")
        os.replace(temporary, audio_path)
    except (OSError, RuntimeError):
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    return audio_path


async def _create_note_with_audio(
    settings: Settings,
    *,
    wav_path: Path,
    digest: str,
    original_filename: str,
    device_name: str,
    transcript: str,
    formatted: str | None,
    title: str,
    wav: dict[str, int],
    profile_id: str = "default",
) -> Path:
    note_path = _new_note_path(
        settings,
        digest=digest,
        original_filename=original_filename,
        profile_id=profile_id,
        title=title,
    )
    audio_path = await _export_note_audio(settings, wav_path, note_path)
    return _write_note(
        settings,
        digest=digest,
        original_filename=original_filename,
        device_name=device_name,
        transcript=transcript,
        formatted=formatted,
        title=title,
        wav=wav,
        profile_id=profile_id,
        note_path=note_path,
        audio_filename=audio_path.name if audio_path else "",
    )


def create_app(
    settings: Settings | None = None, codex_backend: CodexBackend | None = None
) -> FastAPI:
    settings = settings or Settings.from_env()
    store = NoteStore(settings.database_path)
    transcriber = Transcriber(settings)
    formatter = AgentFormatter(settings)
    speech = SpeechSynthesizer(settings)
    codex = codex_backend or (
        CodexAppServer(settings.codex_executable)
        if settings.codex_enabled
        else CodexDisabled()
    )
    settings.spool_root.mkdir(parents=True, exist_ok=True)
    work_event = asyncio.Event()
    worker_task: asyncio.Task[None] | None = None
    mdns_process: asyncio.subprocess.Process | None = None

    async def process_voice_job(job_id: str) -> None:
        job = store.get_voice_job(job_id)
        if not job or job.status not in {"accepted", "retrying"}:
            return
        audio_path = Path(job.audio_path)
        stage = "validating"

        def is_canceled() -> bool:
            current = store.get_voice_job(job.id)
            return current is not None and current.status == "canceled"

        async def cancel_codex_job_if_needed(codex_job_id: str) -> None:
            """Stop a remote Codex turn when the local voice job is canceled.

            Cancellation can race with ``start_turn``: the device may cancel
            the queue item while the App Server is still accepting the turn.
            In that case the local job must remain canceled, but the remote
            turn should not continue consuming work or produce an orphaned
            response.  Best-effort cancellation is intentional here because a
            transport failure must not turn a user-requested cancellation into
            a failed voice job.
            """
            if not codex_job_id:
                return
            try:
                # Cancellation is best effort, but it must not hold the
                # single voice worker behind a dead App Server transport.
                await asyncio.wait_for(codex.cancel_job(codex_job_id), timeout=5)
            except Exception:
                return

        try:
            if not audio_path.is_file():
                raise RuntimeError("Audio file is missing from gateway spool")
            job = store.update_voice_job(
                job.id,
                status="processing",
                stage=stage,
                progress=10,
                error="",
                attempts=job.attempts + 1,
            )
            wav_metadata = _validate_wav(audio_path)
            if is_canceled():
                return

            transcript = job.transcript
            if not transcript:
                stage = "transcribing"
                store.update_voice_job(job.id, stage=stage, progress=25)
                transcript = await transcriber.transcribe(
                    audio_path, job.original_filename
                )
                if is_canceled():
                    return
                store.update_voice_job(job.id, transcript=transcript, progress=55)

            if is_canceled():
                return

            title = job.title
            suggested_filename = job.suggested_filename
            if not title:
                stage = "titling"
                store.update_voice_job(job.id, stage=stage, progress=60)
                title = _safe_title(await formatter.title(transcript))
                if is_canceled():
                    return
            if not suggested_filename:
                suggested_filename = _suggested_recording_filename(title)
                store.update_voice_job(
                    job.id,
                    title=title,
                    suggested_filename=suggested_filename,
                )
                if is_canceled():
                    return

            note_path: Path | None = Path(job.note_path) if job.note_path else None
            formatted = job.formatted
            if job.destination in {"note", "both"}:
                if not note_path or not note_path.is_file():
                    if not formatted:
                        stage = "formatting"
                        store.update_voice_job(job.id, stage=stage, progress=68)
                        profile = get_profile(job.profile)
                        formatted = (
                            await formatter.format(transcript, profile.instruction)
                            or ""
                        )
                        if is_canceled():
                            return
                        store.update_voice_job(job.id, formatted=formatted)
                    stage = "saving"
                    store.update_voice_job(job.id, stage=stage, progress=78)
                    note_path = await _create_note_with_audio(
                        settings,
                        wav_path=audio_path,
                        digest=job.digest,
                        original_filename=job.original_filename,
                        device_name=job.device_name,
                        transcript=transcript,
                        formatted=formatted or None,
                        title=title,
                        wav=wav_metadata,
                        profile_id=job.profile,
                    )
                    if is_canceled():
                        return
                store.update_voice_job(job.id, note_path=str(note_path), progress=85)

            if is_canceled():
                return

            codex_job_id = job.codex_job_id
            if job.destination in {"codex", "both"} and not codex_job_id:
                stage = "sending_codex"
                store.update_voice_job(job.id, stage=stage, progress=90)
                codex_job = await codex.start_turn(job.thread_id, transcript)
                if is_canceled():
                    await cancel_codex_job_if_needed(codex_job.id)
                    return
                codex_job_id = codex_job.id
                store.update_voice_job(job.id, codex_job_id=codex_job_id)

            if is_canceled():
                await cancel_codex_job_if_needed(codex_job_id)
                return
            existing_delivery = store.find_delivery(job.route_key)
            if not existing_delivery:
                store.add_delivery(
                    route_key=job.route_key,
                    digest=job.digest,
                    destination=job.destination,
                    thread_id=job.thread_id,
                    note_path=note_path,
                    transcript=transcript,
                    job_id=codex_job_id,
                )
            store.update_voice_job_unless_canceled(
                job.id,
                status="completed",
                stage="completed",
                progress=100,
                error="",
                transcript=transcript,
                formatted=formatted,
                title=title,
                suggested_filename=suggested_filename,
                note_path=str(note_path) if note_path else "",
                codex_job_id=codex_job_id,
            )
            # Keep the source in the private spool so another profile can reuse
            # the transcript/audio without retranscribing. A retention command
            # can prune completed jobs after their configured history window.
        except asyncio.CancelledError:
            raise
        except Exception as error:
            current = store.get_voice_job(job_id)
            if current and current.status == "canceled":
                return
            store.update_voice_job(
                job_id,
                status="failed",
                stage=stage,
                error=str(error)[:500],
            )

    async def process_pending_voice_jobs() -> int:
        processed = 0
        while job := store.next_voice_job():
            await process_voice_job(job.id)
            processed += 1
        return processed

    async def voice_worker() -> None:
        while True:
            await work_event.wait()
            work_event.clear()
            await process_pending_voice_jobs()

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        nonlocal worker_task, mdns_process
        store.recover_voice_jobs()
        worker_task = asyncio.create_task(voice_worker())
        work_event.set()
        dns_sd = shutil.which("dns-sd")
        if settings.advertise_mdns and dns_sd:
            mdns_process = await asyncio.create_subprocess_exec(
                dns_sd,
                "-R",
                "Cardputer Agent Gateway",
                "_cardputer-agent._tcp",
                "local.",
                str(settings.advertise_port),
                "scheme=https",
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL,
            )
        try:
            yield
        finally:
            worker_task.cancel()
            try:
                await worker_task
            except asyncio.CancelledError:
                pass
            if mdns_process and mdns_process.returncode is None:
                mdns_process.terminate()
                await mdns_process.wait()
            await codex.close()

    app = FastAPI(title="Cardputer Agent Gateway", version="0.2.0", lifespan=lifespan)
    app.state.process_pending_voice_jobs = process_pending_voice_jobs
    app.state.store = store

    @app.get("/health")
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    @app.get("/v1/status")
    async def service_status(
        x_device_token: str = Header(default=""),
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        whisper_status, formatter_status = await asyncio.gather(
            transcriber.status(), formatter.status()
        )
        if not settings.codex_enabled:
            codex_status = {
                "status": "disabled",
                "detail": "CODEX_ENABLED is false",
            }
        else:
            try:
                await asyncio.wait_for(codex.list_threads(1), timeout=20)
                codex_status = {"status": "ok", "detail": "app-server ready"}
            except Exception as error:
                codex_status = {"status": "error", "detail": str(error)[:160]}
        speech_status = speech.status()
        healthy = whisper_status["status"] == "ok" and all(
            item["status"] in {"ok", "disabled"}
            for item in (formatter_status, codex_status, speech_status)
        )
        return {
            "status": "ok" if healthy else "degraded",
            "gateway": {"status": "ok", "detail": "authenticated"},
            "whisper": whisper_status,
            "formatter": formatter_status,
            "codex": codex_status,
            "speech": speech_status,
            "voice_queue": {
                "status": "ok",
                "pending": sum(
                    job.status in {"accepted", "retrying", "processing"}
                    for job in store.list_voice_jobs(limit=100)
                ),
                "failed": sum(
                    job.status == "failed" for job in store.list_voice_jobs(limit=100)
                ),
            },
        }

    @app.get("/v1/profiles")
    async def voice_profiles(
        x_device_token: str = Header(default=""),
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        return {
            "data": [
                {"id": item.id, "name": item.name, "folder": item.folder}
                for item in PROFILES.values()
            ]
        }

    @app.post("/v1/voice/jobs")
    async def create_voice_job(
        request: Request,
        x_device_token: str = Header(default=""),
        x_voice_filename: str = Header(default="recording.wav"),
        x_device_name: str = Header(default="cardputer-adv"),
        x_voice_destination: str = Header(default="note"),
        x_codex_thread_id: str = Header(default=""),
        x_voice_profile: str = Header(default="default"),
    ) -> JSONResponse:
        _require_token(x_device_token, settings.device_token)
        destination = x_voice_destination.strip().lower()
        if destination not in VOICE_DESTINATIONS:
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Invalid destination")
        thread_id = x_codex_thread_id.strip()
        if destination in {"codex", "both"} and not SAFE_THREAD_ID.fullmatch(thread_id):
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Codex thread is required")
        profile = x_voice_profile.strip().lower() or "default"
        if not SAFE_PROFILE.fullmatch(profile):
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Invalid voice profile")
        content_length = _content_length(request)
        if content_length <= 44 or content_length > settings.max_upload_bytes:
            raise HTTPException(
                status.HTTP_413_REQUEST_ENTITY_TOO_LARGE, "Invalid upload size"
            )

        filename = _safe_name(x_voice_filename, "recording.wav")
        device_name = _safe_name(x_device_name, "cardputer-adv")
        digest = hashlib.sha256()
        bytes_written = 0
        fd, temporary_name = tempfile.mkstemp(
            prefix="voice-job-", suffix=".part", dir=settings.spool_root
        )
        temporary_path = Path(temporary_name)
        try:
            with os.fdopen(fd, "wb") as target:
                async for chunk in request.stream():
                    bytes_written += len(chunk)
                    if bytes_written > settings.max_upload_bytes:
                        raise HTTPException(
                            status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
                            "Upload too large",
                        )
                    digest.update(chunk)
                    target.write(chunk)
            if bytes_written != content_length:
                raise HTTPException(status.HTTP_400_BAD_REQUEST, "Incomplete upload")
            _validate_wav(temporary_path)
            sha256 = digest.hexdigest()
            route_key = f"{sha256}:{destination}:{thread_id}:{profile}"
            existing = store.find_voice_job_by_route(route_key)
            if existing:
                return JSONResponse(
                    status_code=(
                        status.HTTP_200_OK
                        if existing.status == "completed"
                        else status.HTTP_202_ACCEPTED
                    ),
                    content=existing.public(include_transcript=False),
                )

            job_id = uuid.uuid4().hex
            audio_path = settings.spool_root / f"{job_id}.wav"
            os.replace(temporary_path, audio_path)
            try:
                job = store.create_voice_job(
                    job_id=job_id,
                    route_key=route_key,
                    digest=sha256,
                    original_filename=filename,
                    device_name=device_name,
                    destination=destination,
                    thread_id=thread_id,
                    profile=profile,
                    audio_path=audio_path,
                )
            except sqlite3.IntegrityError:
                # Two retries can pass the read-before-insert check together.
                # The route key is the idempotency boundary, so return the
                # winner instead of exposing SQLite's UNIQUE constraint as a
                # gateway 500 (which makes the Cardputer retry indefinitely).
                audio_path.unlink(missing_ok=True)
                existing = store.find_voice_job_by_route(route_key)
                if existing is None:
                    raise
                return JSONResponse(
                    status_code=(
                        status.HTTP_200_OK
                        if existing.status == "completed"
                        else status.HTTP_202_ACCEPTED
                    ),
                    content=existing.public(include_transcript=False),
                )
            work_event.set()
            return JSONResponse(
                status_code=status.HTTP_202_ACCEPTED,
                content=job.public(include_transcript=False),
            )
        finally:
            temporary_path.unlink(missing_ok=True)

    @app.get("/v1/voice/jobs")
    async def list_voice_jobs(
        limit: int = 30,
        device_name: str = "",
        x_device_token: str = Header(default=""),
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        # Keep a malformed/overly large UI request from forcing an unbounded
        # SQLite query or a huge JSON response on the small device.
        jobs = store.list_voice_jobs(
            device_name=device_name, limit=max(1, min(limit, 100))
        )
        return {"data": [job.public(include_transcript=False) for job in jobs]}

    @app.post("/v1/voice/jobs/retry-failed", status_code=202)
    async def retry_failed_voice_jobs(
        x_device_token: str = Header(default=""),
        x_device_name: str = Header(default=""),
    ) -> dict[str, object]:
        _require_token(x_device_token, settings.device_token)
        device_name = _safe_name(x_device_name, "cardputer-adv")
        retried: list[str] = []
        for job in store.list_voice_jobs(device_name=device_name, limit=100):
            if job.status != "failed" or not Path(job.audio_path).is_file():
                continue
            store.update_voice_job(
                job.id, status="retrying", stage="queued", progress=5, error=""
            )
            retried.append(job.id)
        if retried:
            work_event.set()
        return {"status": "retrying" if retried else "ok", "count": len(retried)}

    @app.get("/v1/voice/jobs/{job_id}")
    async def get_voice_job(
        job_id: str, x_device_token: str = Header(default="")
    ) -> dict[str, object]:
        _require_token(x_device_token, settings.device_token)
        job = store.get_voice_job(job_id)
        if not job:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "Voice job not found")
        return job.public()

    @app.post("/v1/voice/jobs/{job_id}/retry", status_code=202)
    async def retry_voice_job(
        job_id: str, x_device_token: str = Header(default="")
    ) -> dict[str, object]:
        _require_token(x_device_token, settings.device_token)
        job = store.get_voice_job(job_id)
        if not job:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "Voice job not found")
        if job.status not in {"failed", "canceled"}:
            raise HTTPException(status.HTTP_409_CONFLICT, "Voice job is not retryable")
        if not Path(job.audio_path).is_file():
            raise HTTPException(status.HTTP_410_GONE, "Audio is no longer available")
        job = store.update_voice_job(
            job.id, status="retrying", stage="queued", progress=5, error=""
        )
        work_event.set()
        return job.public(include_transcript=False)

    @app.post("/v1/voice/jobs/{job_id}/cancel")
    async def cancel_voice_job(
        job_id: str, x_device_token: str = Header(default="")
    ) -> dict[str, object]:
        _require_token(x_device_token, settings.device_token)
        job = store.get_voice_job(job_id)
        if not job:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "Voice job not found")
        if job.status in {"completed", "canceled"}:
            return job.public(include_transcript=False)
        job = store.update_voice_job(
            job.id, status="canceled", stage="canceled", error="Canceled by device"
        )
        return job.public(include_transcript=False)

    @app.post("/v1/voice/jobs/{job_id}/reprocess", status_code=202)
    async def reprocess_voice_job(
        job_id: str,
        body: ReprocessVoiceJob,
        x_device_token: str = Header(default=""),
    ) -> dict[str, object]:
        _require_token(x_device_token, settings.device_token)
        source = store.get_voice_job(job_id)
        if not source:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "Voice job not found")
        if not source.transcript:
            raise HTTPException(
                status.HTTP_409_CONFLICT, "Transcript is not available yet"
            )
        profile = body.profile.strip().lower() or source.profile
        destination = body.destination.strip().lower() or source.destination
        thread_id = body.thread_id.strip() or source.thread_id
        if not SAFE_PROFILE.fullmatch(profile):
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Invalid voice profile")
        if destination not in VOICE_DESTINATIONS:
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Invalid destination")
        if destination in {"codex", "both"} and not SAFE_THREAD_ID.fullmatch(thread_id):
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Codex thread is required")
        route_key = f"{source.digest}:{destination}:{thread_id}:{profile}"
        existing = store.find_voice_job_by_route(route_key)
        if existing:
            return existing.public(include_transcript=False)
        if not Path(source.audio_path).is_file():
            raise HTTPException(status.HTTP_410_GONE, "Source audio was pruned")
        new_id = uuid.uuid4().hex
        job = store.create_voice_job(
            job_id=new_id,
            route_key=route_key,
            digest=source.digest,
            original_filename=source.original_filename,
            device_name=source.device_name,
            destination=destination,
            thread_id=thread_id,
            profile=profile,
            audio_path=Path(source.audio_path),
        )
        job = store.update_voice_job(
            new_id,
            transcript=source.transcript,
            title=source.title,
            suggested_filename=source.suggested_filename,
        )
        work_event.set()
        return job.public(include_transcript=False)

    @app.post("/v1/voice-notes")
    async def voice_note(
        request: Request,
        x_device_token: str = Header(default=""),
        x_voice_filename: str = Header(default="recording.wav"),
        x_device_name: str = Header(default="cardputer-adv"),
    ) -> JSONResponse:
        _require_token(x_device_token, settings.device_token)
        content_length = _content_length(request)
        if content_length <= 44 or content_length > settings.max_upload_bytes:
            raise HTTPException(
                status.HTTP_413_REQUEST_ENTITY_TOO_LARGE, "Invalid upload size"
            )

        filename = _safe_name(x_voice_filename, "recording.wav")
        device_name = _safe_name(x_device_name, "cardputer-adv")
        digest = hashlib.sha256()
        bytes_written = 0
        fd, temporary_name = tempfile.mkstemp(
            prefix="voice-", suffix=".wav", dir=settings.spool_root
        )
        temporary_path = Path(temporary_name)
        try:
            with os.fdopen(fd, "wb") as target:
                async for chunk in request.stream():
                    bytes_written += len(chunk)
                    if bytes_written > settings.max_upload_bytes:
                        raise HTTPException(
                            status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
                            "Upload too large",
                        )
                    digest.update(chunk)
                    target.write(chunk)
            if bytes_written != content_length:
                raise HTTPException(status.HTTP_400_BAD_REQUEST, "Incomplete upload")

            sha256 = digest.hexdigest()
            existing = store.find(sha256)
            if existing:
                return JSONResponse(
                    status_code=status.HTTP_200_OK,
                    content={
                        "status": "duplicate",
                        "sha256": sha256,
                        "note_path": existing.note_path,
                    },
                )

            wav_metadata = _validate_wav(temporary_path)
            try:
                transcript = await transcriber.transcribe(temporary_path, filename)
                formatted = await formatter.format(transcript)
                title = _safe_title(await formatter.title(transcript))
            except VOICE_PROCESSING_ERRORS as error:
                raise _voice_processing_http_error(error) from error
            try:
                note_path = await _create_note_with_audio(
                    settings,
                    wav_path=temporary_path,
                    digest=sha256,
                    original_filename=filename,
                    device_name=device_name,
                    transcript=transcript,
                    formatted=formatted,
                    title=title,
                    wav=wav_metadata,
                )
            except VOICE_PROCESSING_ERRORS as error:
                raise _voice_processing_http_error(error) from error
            stored = store.add(
                digest=sha256,
                original_filename=filename,
                device_name=device_name,
                note_path=note_path,
                transcript=transcript,
            )
            return JSONResponse(
                status_code=status.HTTP_201_CREATED,
                content={
                    "status": "created",
                    "sha256": stored.digest,
                    "note_path": stored.note_path,
                },
            )
        finally:
            temporary_path.unlink(missing_ok=True)

    @app.post("/v1/voice")
    async def routed_voice(
        request: Request,
        x_device_token: str = Header(default=""),
        x_voice_filename: str = Header(default="recording.wav"),
        x_device_name: str = Header(default="cardputer-adv"),
        x_voice_destination: str = Header(default="note"),
        x_codex_thread_id: str = Header(default=""),
        x_compact_response: str = Header(default="false"),
        x_transcript_max_chars: int = Header(default=0),
    ) -> JSONResponse:
        _require_token(x_device_token, settings.device_token)
        destination = x_voice_destination.strip().lower()
        if destination not in VOICE_DESTINATIONS:
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Invalid destination")
        thread_id = x_codex_thread_id.strip()
        transcript_limit = max(0, min(x_transcript_max_chars, 12_000))

        def add_transcript(content: dict[str, Any], transcript: str) -> None:
            if x_compact_response.lower() in {"1", "true", "yes"}:
                return
            content["transcript"] = (
                transcript[:transcript_limit] if transcript_limit else transcript
            )
            content["transcript_complete"] = (
                not transcript_limit or len(transcript) <= transcript_limit
            )

        if destination in {"codex", "both"} and not SAFE_THREAD_ID.fullmatch(thread_id):
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Codex thread is required")

        content_length = _content_length(request)
        if content_length <= 44 or content_length > settings.max_upload_bytes:
            raise HTTPException(
                status.HTTP_413_REQUEST_ENTITY_TOO_LARGE, "Invalid upload size"
            )
        filename = _safe_name(x_voice_filename, "recording.wav")
        device_name = _safe_name(x_device_name, "cardputer-adv")
        digest = hashlib.sha256()
        bytes_written = 0
        fd, temporary_name = tempfile.mkstemp(
            prefix="voice-", suffix=".wav", dir=settings.spool_root
        )
        temporary_path = Path(temporary_name)
        try:
            with os.fdopen(fd, "wb") as target:
                async for chunk in request.stream():
                    bytes_written += len(chunk)
                    if bytes_written > settings.max_upload_bytes:
                        raise HTTPException(
                            status.HTTP_413_REQUEST_ENTITY_TOO_LARGE,
                            "Upload too large",
                        )
                    digest.update(chunk)
                    target.write(chunk)
            if bytes_written != content_length:
                raise HTTPException(status.HTTP_400_BAD_REQUEST, "Incomplete upload")
            sha256 = digest.hexdigest()
            route_key = f"{sha256}:{destination}:{thread_id}"
            existing_delivery = store.find_delivery(route_key)
            if existing_delivery:
                content: dict[str, Any] = {
                    "status": "duplicate",
                    "sha256": sha256,
                    "destination": destination,
                    "note_path": existing_delivery.note_path or None,
                    "job_id": existing_delivery.job_id or None,
                }
                add_transcript(content, existing_delivery.transcript)
                return JSONResponse(
                    status_code=status.HTTP_200_OK,
                    content=content,
                )

            wav_metadata = _validate_wav(temporary_path)
            try:
                transcript = await transcriber.transcribe(temporary_path, filename)
                title = _safe_title(await formatter.title(transcript))
            except VOICE_PROCESSING_ERRORS as error:
                raise _voice_processing_http_error(error) from error
            note_path: Path | None = None
            if destination in {"note", "both"}:
                existing_note = store.find(sha256)
                if existing_note:
                    note_path = Path(existing_note.note_path)
                else:
                    try:
                        formatted = await formatter.format(transcript)
                    except VOICE_PROCESSING_ERRORS as error:
                        raise _voice_processing_http_error(error) from error
                    try:
                        note_path = await _create_note_with_audio(
                            settings,
                            wav_path=temporary_path,
                            digest=sha256,
                            original_filename=filename,
                            device_name=device_name,
                            transcript=transcript,
                            formatted=formatted,
                            title=title,
                            wav=wav_metadata,
                        )
                    except VOICE_PROCESSING_ERRORS as error:
                        raise _voice_processing_http_error(error) from error
                    store.add(
                        digest=sha256,
                        original_filename=filename,
                        device_name=device_name,
                        note_path=note_path,
                        transcript=transcript,
                    )

            job_id = ""
            if destination in {"codex", "both"}:
                try:
                    job = await codex.start_turn(thread_id, transcript)
                except VOICE_PROCESSING_ERRORS as error:
                    raise _voice_processing_http_error(error) from error
                job_id = job.id
            stored = store.add_delivery(
                route_key=route_key,
                digest=sha256,
                destination=destination,
                thread_id=thread_id,
                note_path=note_path,
                transcript=transcript,
                job_id=job_id,
            )
            content = {
                "status": "created",
                "sha256": sha256,
                "destination": destination,
                "note_path": stored.note_path or None,
                "job_id": stored.job_id or None,
            }
            add_transcript(content, transcript)
            return JSONResponse(
                status_code=status.HTTP_201_CREATED,
                content=content,
            )
        finally:
            temporary_path.unlink(missing_ok=True)

    @app.get("/v1/codex/chats")
    async def codex_chats(
        limit: int = 10, x_device_token: str = Header(default="")
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        try:
            threads = await codex.list_threads(limit)
            summaries = [_thread_summary(thread) for thread in threads]
        except VOICE_PROCESSING_ERRORS as error:
            raise _voice_processing_http_error(error) from error
        return {"data": summaries}

    @app.post("/v1/codex/chats")
    async def create_codex_chat(
        body: NewCodexThread, x_device_token: str = Header(default="")
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        cwd = body.cwd or settings.codex_default_cwd
        if settings.codex_default_cwd and cwd != settings.codex_default_cwd:
            raise HTTPException(
                status.HTTP_403_FORBIDDEN, "Unsupported working directory"
            )
        try:
            thread = await codex.start_thread(cwd)
            return _thread_summary(thread)
        except VOICE_PROCESSING_ERRORS as error:
            raise _voice_processing_http_error(error) from error

    @app.get("/v1/codex/chats/{thread_id}")
    async def codex_chat(
        thread_id: str, x_device_token: str = Header(default="")
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        if not SAFE_THREAD_ID.fullmatch(thread_id):
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Invalid thread id")
        try:
            thread = await codex.read_thread(thread_id)
            result = _thread_summary(thread)
            result["messages"] = _thread_messages(thread)
        except VOICE_PROCESSING_ERRORS as error:
            raise _voice_processing_http_error(error) from error
        return result

    @app.get("/v1/codex/chats/{thread_id}/speech")
    async def codex_chat_speech(
        thread_id: str,
        scope: str = "last",
        x_device_token: str = Header(default=""),
    ) -> Response:
        _require_token(x_device_token, settings.device_token)
        if not SAFE_THREAD_ID.fullmatch(thread_id):
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Invalid thread id")
        if scope not in {"last", "conversation"}:
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Invalid speech scope")
        try:
            thread = await codex.read_thread(thread_id)
            messages = _thread_messages(thread, limit=32)
            if scope == "last":
                text = next(
                    (
                        item["text"]
                        for item in reversed(messages)
                        if item["role"] == "assistant"
                    ),
                    "",
                )
            else:
                text = "\n\n".join(
                    ("Пользователь: " if item["role"] == "user" else "Codex: ")
                    + item["text"]
                    for item in messages
                )
            audio = await speech.synthesize(text)
        except VOICE_PROCESSING_ERRORS as error:
            raise _voice_processing_http_error(error) from error
        return Response(
            content=audio,
            media_type="audio/wav",
            headers={
                "Content-Disposition": 'inline; filename="codex-speech.wav"',
                "Cache-Control": "private, max-age=300",
            },
        )

    @app.post("/v1/codex/chats/{thread_id}/messages", status_code=202)
    async def send_codex_message(
        thread_id: str,
        body: CodexMessage,
        x_device_token: str = Header(default=""),
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        if not SAFE_THREAD_ID.fullmatch(thread_id):
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "Invalid thread id")
        try:
            job = await codex.start_turn(thread_id, body.text)
        except VOICE_PROCESSING_ERRORS as error:
            raise _voice_processing_http_error(error) from error
        return job.public()

    @app.get("/v1/codex/jobs/{job_id}")
    async def codex_job(
        job_id: str, x_device_token: str = Header(default="")
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        job = codex.get_job(job_id)
        if not job:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "Job not found")
        return job.public()

    @app.post("/v1/codex/jobs/{job_id}/approvals/{approval_id}")
    async def answer_codex_approval(
        job_id: str,
        approval_id: str,
        body: CodexApprovalDecision,
        x_device_token: str = Header(default=""),
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        try:
            job = await codex.answer_approval(job_id, approval_id, body.decision)
        except ValueError as error:
            raise HTTPException(status.HTTP_400_BAD_REQUEST, str(error)) from error
        except KeyError as error:
            raise HTTPException(status.HTTP_404_NOT_FOUND, str(error)) from error
        except VOICE_PROCESSING_ERRORS as error:
            raise _voice_processing_http_error(error) from error
        return job.public()

    @app.post("/v1/codex/jobs/{job_id}/cancel")
    async def cancel_codex_job(
        job_id: str, x_device_token: str = Header(default="")
    ) -> dict[str, Any]:
        _require_token(x_device_token, settings.device_token)
        try:
            job = await codex.cancel_job(job_id)
        except KeyError as error:
            raise HTTPException(status.HTTP_404_NOT_FOUND, str(error)) from error
        except VOICE_PROCESSING_ERRORS as error:
            raise _voice_processing_http_error(error) from error
        return job.public()

    return app


def app_factory() -> FastAPI:
    """Uvicorn factory; environment is read only when the server starts."""
    return create_app()
