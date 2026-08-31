from __future__ import annotations

import sqlite3
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path


@dataclass(frozen=True)
class StoredNote:
    digest: str
    note_path: str
    transcript: str


@dataclass(frozen=True)
class StoredDelivery:
    route_key: str
    digest: str
    destination: str
    thread_id: str
    note_path: str
    transcript: str
    job_id: str


@dataclass(frozen=True)
class StoredVoiceJob:
    id: str
    route_key: str
    digest: str
    original_filename: str
    device_name: str
    destination: str
    thread_id: str
    profile: str
    audio_path: str
    status: str
    stage: str
    progress: int
    error: str
    transcript: str
    formatted: str
    title: str
    suggested_filename: str
    note_path: str
    codex_job_id: str
    attempts: int
    created_at: str
    updated_at: str

    def public(self, *, include_transcript: bool = True) -> dict[str, object]:
        result: dict[str, object] = {
            "id": self.id,
            "status": self.status,
            "stage": self.stage,
            "progress": self.progress,
            "error": self.error or None,
            "filename": self.original_filename,
            "destination": self.destination,
            "thread_id": self.thread_id or None,
            "profile": self.profile,
            "title": self.title or None,
            "suggested_filename": self.suggested_filename or None,
            "sha256": self.digest,
            "note_path": self.note_path or None,
            "codex_job_id": self.codex_job_id or None,
            "attempts": self.attempts,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
        }
        if include_transcript:
            result["transcript"] = self.transcript
            result["formatted"] = self.formatted or None
        return result


class NoteStore:
    def __init__(self, database_path: Path):
        self.database_path = database_path
        database_path.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as connection:
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS voice_notes (
                    digest TEXT PRIMARY KEY,
                    original_filename TEXT NOT NULL,
                    device_name TEXT NOT NULL,
                    note_path TEXT NOT NULL,
                    transcript TEXT NOT NULL,
                    created_at TEXT NOT NULL
                )
                """
            )
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS voice_jobs (
                    id TEXT PRIMARY KEY,
                    route_key TEXT NOT NULL UNIQUE,
                    digest TEXT NOT NULL,
                    original_filename TEXT NOT NULL,
                    device_name TEXT NOT NULL,
                    destination TEXT NOT NULL,
                    thread_id TEXT NOT NULL,
                    profile TEXT NOT NULL DEFAULT 'default',
                    audio_path TEXT NOT NULL,
                    status TEXT NOT NULL,
                    stage TEXT NOT NULL,
                    progress INTEGER NOT NULL DEFAULT 0,
                    error TEXT NOT NULL DEFAULT '',
                    transcript TEXT NOT NULL DEFAULT '',
                    formatted TEXT NOT NULL DEFAULT '',
                    title TEXT NOT NULL DEFAULT '',
                    suggested_filename TEXT NOT NULL DEFAULT '',
                    note_path TEXT NOT NULL DEFAULT '',
                    codex_job_id TEXT NOT NULL DEFAULT '',
                    attempts INTEGER NOT NULL DEFAULT 0,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                )
                """
            )
            columns = {
                row[1] for row in connection.execute("PRAGMA table_info(voice_jobs)")
            }
            if "title" not in columns:
                connection.execute(
                    "ALTER TABLE voice_jobs ADD COLUMN title TEXT NOT NULL DEFAULT ''"
                )
            if "suggested_filename" not in columns:
                connection.execute(
                    "ALTER TABLE voice_jobs ADD COLUMN suggested_filename "
                    "TEXT NOT NULL DEFAULT ''"
                )
            connection.execute(
                "CREATE INDEX IF NOT EXISTS voice_jobs_status_idx "
                "ON voice_jobs(status, created_at)"
            )
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS voice_deliveries (
                    route_key TEXT PRIMARY KEY,
                    digest TEXT NOT NULL,
                    destination TEXT NOT NULL,
                    thread_id TEXT NOT NULL,
                    note_path TEXT NOT NULL,
                    transcript TEXT NOT NULL,
                    job_id TEXT NOT NULL,
                    created_at TEXT NOT NULL
                )
                """
            )

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.database_path)
        connection.row_factory = sqlite3.Row
        return connection

    def find(self, digest: str) -> StoredNote | None:
        with self._connect() as connection:
            row = connection.execute(
                "SELECT digest, note_path, transcript FROM voice_notes WHERE digest = ?",
                (digest,),
            ).fetchone()
        return StoredNote(**dict(row)) if row else None

    def add(
        self,
        *,
        digest: str,
        original_filename: str,
        device_name: str,
        note_path: Path,
        transcript: str,
    ) -> StoredNote:
        with self._connect() as connection:
            connection.execute(
                """
                INSERT INTO voice_notes (
                    digest, original_filename, device_name, note_path,
                    transcript, created_at
                ) VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    digest,
                    original_filename,
                    device_name,
                    str(note_path),
                    transcript,
                    datetime.now(UTC).isoformat(),
                ),
            )
        return StoredNote(digest, str(note_path), transcript)

    def find_delivery(self, route_key: str) -> StoredDelivery | None:
        with self._connect() as connection:
            row = connection.execute(
                """
                SELECT route_key, digest, destination, thread_id, note_path,
                       transcript, job_id
                FROM voice_deliveries WHERE route_key = ?
                """,
                (route_key,),
            ).fetchone()
        return StoredDelivery(**dict(row)) if row else None

    def add_delivery(
        self,
        *,
        route_key: str,
        digest: str,
        destination: str,
        thread_id: str,
        note_path: Path | None,
        transcript: str,
        job_id: str,
    ) -> StoredDelivery:
        stored = StoredDelivery(
            route_key=route_key,
            digest=digest,
            destination=destination,
            thread_id=thread_id,
            note_path=str(note_path) if note_path else "",
            transcript=transcript,
            job_id=job_id,
        )
        with self._connect() as connection:
            connection.execute(
                """
                INSERT INTO voice_deliveries (
                    route_key, digest, destination, thread_id, note_path,
                    transcript, job_id, created_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    stored.route_key,
                    stored.digest,
                    stored.destination,
                    stored.thread_id,
                    stored.note_path,
                    stored.transcript,
                    stored.job_id,
                    datetime.now(UTC).isoformat(),
                ),
            )
        return stored

    def create_voice_job(
        self,
        *,
        job_id: str,
        route_key: str,
        digest: str,
        original_filename: str,
        device_name: str,
        destination: str,
        thread_id: str,
        profile: str,
        audio_path: Path,
    ) -> StoredVoiceJob:
        now = datetime.now(UTC).isoformat()
        with self._connect() as connection:
            connection.execute(
                """
                INSERT INTO voice_jobs (
                    id, route_key, digest, original_filename, device_name,
                    destination, thread_id, profile, audio_path, status, stage,
                    progress, created_at, updated_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'accepted', 'queued', 5, ?, ?)
                """,
                (
                    job_id,
                    route_key,
                    digest,
                    original_filename,
                    device_name,
                    destination,
                    thread_id,
                    profile,
                    str(audio_path),
                    now,
                    now,
                ),
            )
        job = self.get_voice_job(job_id)
        assert job is not None
        return job

    def get_voice_job(self, job_id: str) -> StoredVoiceJob | None:
        with self._connect() as connection:
            row = connection.execute(
                "SELECT * FROM voice_jobs WHERE id = ?", (job_id,)
            ).fetchone()
        return StoredVoiceJob(**dict(row)) if row else None

    def find_voice_job_by_route(self, route_key: str) -> StoredVoiceJob | None:
        with self._connect() as connection:
            row = connection.execute(
                "SELECT * FROM voice_jobs WHERE route_key = ?", (route_key,)
            ).fetchone()
        return StoredVoiceJob(**dict(row)) if row else None

    def list_voice_jobs(
        self, *, device_name: str = "", limit: int = 30
    ) -> list[StoredVoiceJob]:
        query = "SELECT * FROM voice_jobs"
        values: tuple[object, ...]
        if device_name:
            query += " WHERE device_name = ?"
            values = (device_name, max(1, min(limit, 100)))
        else:
            values = (max(1, min(limit, 100)),)
        query += " ORDER BY created_at DESC LIMIT ?"
        with self._connect() as connection:
            rows = connection.execute(query, values).fetchall()
        return [StoredVoiceJob(**dict(row)) for row in rows]

    def next_voice_job(self) -> StoredVoiceJob | None:
        with self._connect() as connection:
            row = connection.execute(
                """
                SELECT * FROM voice_jobs
                WHERE status IN ('accepted', 'retrying')
                ORDER BY created_at ASC LIMIT 1
                """
            ).fetchone()
        return StoredVoiceJob(**dict(row)) if row else None

    def update_voice_job(self, job_id: str, **changes: object) -> StoredVoiceJob:
        allowed = {
            "status",
            "stage",
            "progress",
            "error",
            "transcript",
            "formatted",
            "title",
            "suggested_filename",
            "note_path",
            "codex_job_id",
            "attempts",
            "audio_path",
        }
        unknown = set(changes) - allowed
        if unknown:
            raise ValueError(f"Unsupported voice job fields: {sorted(unknown)}")
        changes["updated_at"] = datetime.now(UTC).isoformat()
        columns = ", ".join(f"{name} = ?" for name in changes)
        values = tuple(changes.values()) + (job_id,)
        with self._connect() as connection:
            connection.execute(f"UPDATE voice_jobs SET {columns} WHERE id = ?", values)
        job = self.get_voice_job(job_id)
        if job is None:
            raise KeyError(job_id)
        return job

    def update_voice_job_unless_canceled(
        self, job_id: str, **changes: object
    ) -> StoredVoiceJob:
        """Update a job unless a concurrent request already canceled it.

        Voice processing performs several network and filesystem steps.  A
        cancellation request can arrive after the worker's last status check,
        so the final ``completed`` write must be conditional in the same SQL
        statement; a separate read/check would still leave a race window.
        """
        allowed = {
            "status",
            "stage",
            "progress",
            "error",
            "transcript",
            "formatted",
            "title",
            "suggested_filename",
            "note_path",
            "codex_job_id",
            "attempts",
            "audio_path",
        }
        unknown = set(changes) - allowed
        if unknown:
            raise ValueError(f"Unsupported voice job fields: {sorted(unknown)}")
        changes["updated_at"] = datetime.now(UTC).isoformat()
        columns = ", ".join(f"{name} = ?" for name in changes)
        values = tuple(changes.values()) + (job_id,)
        with self._connect() as connection:
            connection.execute(
                f"UPDATE voice_jobs SET {columns} "
                "WHERE id = ? AND status <> 'canceled'",
                values,
            )
            row = connection.execute(
                "SELECT * FROM voice_jobs WHERE id = ?", (job_id,)
            ).fetchone()
        if row is None:
            raise KeyError(job_id)
        return StoredVoiceJob(**dict(row))

    def recover_voice_jobs(self) -> None:
        """Return interrupted work to the queue after a gateway restart."""
        now = datetime.now(UTC).isoformat()
        with self._connect() as connection:
            connection.execute(
                """
                UPDATE voice_jobs
                SET status = 'retrying', stage = 'queued',
                    error = 'Gateway restarted; resuming', updated_at = ?
                WHERE status = 'processing'
                """,
                (now,),
            )
