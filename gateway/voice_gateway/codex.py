from __future__ import annotations

import asyncio
import json
import shutil
import uuid
from dataclasses import dataclass, field
from typing import Any, Protocol


APPROVAL_METHODS = {
    "item/commandExecution/requestApproval",
    "item/fileChange/requestApproval",
}
APPROVAL_DECISIONS = {"accept", "acceptForSession", "decline", "cancel"}


@dataclass
class CodexApproval:
    id: str
    rpc_id: str | int
    method: str
    thread_id: str
    turn_id: str
    reason: str = ""
    command: str = ""
    cwd: str = ""

    def public(self) -> dict[str, str]:
        return {
            "id": self.id,
            "kind": "command" if "commandExecution" in self.method else "file",
            "reason": self.reason,
            "command": self.command,
            "cwd": self.cwd,
        }


@dataclass
class CodexJob:
    id: str
    thread_id: str
    turn_id: str = ""
    status: str = "queued"
    text: str = ""
    error: str = ""
    approvals: dict[str, CodexApproval] = field(default_factory=dict)

    def public(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "thread_id": self.thread_id,
            "turn_id": self.turn_id or None,
            "status": self.status,
            "text": self.text,
            "error": self.error or None,
            "approvals": [item.public() for item in self.approvals.values()],
        }


class CodexBackend(Protocol):
    async def list_threads(self, limit: int = 10) -> list[dict[str, Any]]: ...

    async def read_thread(self, thread_id: str) -> dict[str, Any]: ...

    async def start_thread(self, cwd: str = "") -> dict[str, Any]: ...

    async def start_turn(self, thread_id: str, text: str) -> CodexJob: ...

    def get_job(self, job_id: str) -> CodexJob | None: ...

    async def answer_approval(
        self, job_id: str, approval_id: str, decision: str
    ) -> CodexJob: ...

    async def cancel_job(self, job_id: str) -> CodexJob: ...

    async def close(self) -> None: ...


class CodexDisabled:
    """Backend used when Codex integration is intentionally disabled."""

    async def _unavailable(self, *_: Any, **__: Any) -> Any:
        raise RuntimeError("Codex integration is disabled")

    list_threads = _unavailable
    read_thread = _unavailable
    start_thread = _unavailable
    start_turn = _unavailable
    answer_approval = _unavailable
    cancel_job = _unavailable

    def get_job(self, job_id: str) -> None:
        return None

    async def close(self) -> None:
        return None


class CodexAppServer:
    """Small JSON-RPC client for the local `codex app-server` process."""

    def __init__(self, executable: str = "codex"):
        self.executable = executable
        self.process: asyncio.subprocess.Process | None = None
        self.reader_task: asyncio.Task[None] | None = None
        self.stderr_task: asyncio.Task[None] | None = None
        self.pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self.jobs: dict[str, CodexJob] = {}
        self.turn_jobs: dict[str, str] = {}
        self.starting_thread_jobs: dict[str, str] = {}
        self.approval_jobs: dict[str, str] = {}
        self.request_id = 0
        self.write_lock = asyncio.Lock()
        self.start_lock = asyncio.Lock()
        self.stderr_tail = ""

    async def _ensure_started(self) -> None:
        if self.process and self.process.returncode is None:
            return
        async with self.start_lock:
            if self.process and self.process.returncode is None:
                return
            executable = shutil.which(self.executable)
            if not executable:
                raise RuntimeError(f"Codex executable not found: {self.executable}")
            self.process = await asyncio.create_subprocess_exec(
                executable,
                "app-server",
                "--listen",
                "stdio://",
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                limit=16 * 1024 * 1024,
            )
            self.reader_task = asyncio.create_task(self._read_stdout())
            self.stderr_task = asyncio.create_task(self._read_stderr())
            await self._request(
                "initialize",
                {
                    "clientInfo": {
                        "name": "cardputer_agent_console",
                        "title": "Cardputer Agent Console",
                        "version": "0.1.0",
                    },
                    "capabilities": {"experimentalApi": True},
                },
            )
            await self._notify("initialized", {})

    async def _read_stderr(self) -> None:
        assert self.process and self.process.stderr
        while line := await self.process.stderr.readline():
            self.stderr_tail = line.decode("utf-8", "replace").strip()[-1000:]

    async def _read_stdout(self) -> None:
        assert self.process and self.process.stdout
        while line := await self.process.stdout.readline():
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "id" in message and "method" not in message:
                future = self.pending.pop(message["id"], None)
                if future and not future.done():
                    if "error" in message:
                        future.set_exception(
                            RuntimeError(message["error"].get("message", "Codex error"))
                        )
                    else:
                        future.set_result(message.get("result", {}))
                continue
            if "id" in message and message.get("method") in APPROVAL_METHODS:
                self._receive_approval(message)
                continue
            self._receive_notification(message)

        detail = self.stderr_tail or "Codex App Server stopped"
        for future in self.pending.values():
            if not future.done():
                future.set_exception(RuntimeError(detail))
        self.pending.clear()

    async def _send(self, message: dict[str, Any]) -> None:
        if not self.process or not self.process.stdin:
            raise RuntimeError("Codex App Server is not running")
        encoded = (json.dumps(message, separators=(",", ":")) + "\n").encode()
        async with self.write_lock:
            self.process.stdin.write(encoded)
            await self.process.stdin.drain()

    async def _request(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        self.request_id += 1
        request_id = self.request_id
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        await self._send({"method": method, "id": request_id, "params": params})
        return await asyncio.wait_for(future, timeout=30)

    async def _notify(self, method: str, params: dict[str, Any]) -> None:
        await self._send({"method": method, "params": params})

    def _receive_notification(self, message: dict[str, Any]) -> None:
        method = message.get("method", "")
        params = message.get("params") or {}
        turn_id = params.get("turnId") or (params.get("turn") or {}).get("id")
        if method == "turn/started" and turn_id:
            thread_id = params.get("threadId") or (params.get("turn") or {}).get(
                "threadId", ""
            )
            starting_job_id = self.starting_thread_jobs.get(thread_id)
            if not starting_job_id and len(self.starting_thread_jobs) == 1:
                starting_job_id = next(iter(self.starting_thread_jobs.values()))
            if starting_job_id:
                starting_job = self.jobs[starting_job_id]
                starting_job.turn_id = turn_id
                self.turn_jobs[turn_id] = starting_job_id
        job_id = self.turn_jobs.get(turn_id or "")
        if not job_id:
            return
        job = self.jobs[job_id]
        if method == "item/agentMessage/delta":
            job.text += params.get("delta", "")
        elif method == "turn/completed":
            turn = params.get("turn") or {}
            status = turn.get("status", "completed")
            job.status = "completed" if status == "completed" else status
            error = turn.get("error") or {}
            job.error = error.get("message", "")

    def _receive_approval(self, message: dict[str, Any]) -> None:
        params = message.get("params") or {}
        turn_id = params.get("turnId", "")
        job_id = self.turn_jobs.get(turn_id)
        if not job_id:
            return
        approval_id = uuid.uuid4().hex
        command = params.get("command") or ""
        if isinstance(command, list):
            command = " ".join(str(part) for part in command)
        approval = CodexApproval(
            id=approval_id,
            rpc_id=message["id"],
            method=message["method"],
            thread_id=params.get("threadId", ""),
            turn_id=turn_id,
            reason=params.get("reason") or "",
            command=str(command),
            cwd=params.get("cwd") or params.get("grantRoot") or "",
        )
        self.jobs[job_id].approvals[approval_id] = approval
        self.approval_jobs[approval_id] = job_id

    async def list_threads(self, limit: int = 10) -> list[dict[str, Any]]:
        await self._ensure_started()
        result = await self._request(
            "thread/list",
            {
                "limit": max(1, min(limit, 25)),
                "sortKey": "updated_at",
                "sortDirection": "desc",
                "sourceKinds": ["cli", "vscode", "appServer"],
            },
        )
        return result.get("data", [])

    async def read_thread(self, thread_id: str) -> dict[str, Any]:
        await self._ensure_started()
        result = await self._request(
            "thread/read", {"threadId": thread_id, "includeTurns": False}
        )
        thread = result.get("thread", result)
        turns = await self._request(
            "thread/turns/list",
            {
                "threadId": thread_id,
                "limit": 8,
                "sortDirection": "desc",
                "itemsView": "summary",
            },
        )
        thread["turns"] = list(reversed(turns.get("data", [])))
        return thread

    async def start_thread(self, cwd: str = "") -> dict[str, Any]:
        await self._ensure_started()
        params: dict[str, Any] = {
            "approvalPolicy": "unlessTrusted",
            "sandbox": "workspaceWrite",
            "serviceName": "cardputer_agent_console",
        }
        if cwd:
            params["cwd"] = cwd
        result = await self._request("thread/start", params)
        return result.get("thread", result)

    async def start_turn(self, thread_id: str, text: str) -> CodexJob:
        await self._ensure_started()
        if not text.strip():
            raise ValueError("Message text is required")
        await self._request("thread/resume", {"threadId": thread_id})
        job = CodexJob(
            id=uuid.uuid4().hex,
            thread_id=thread_id,
            status="starting",
        )
        self.jobs[job.id] = job
        self.starting_thread_jobs[thread_id] = job.id
        try:
            result = await self._request(
                "turn/start",
                {
                    "threadId": thread_id,
                    "input": [{"type": "text", "text": text}],
                },
            )
        except Exception:
            job.status = "failed"
            self.starting_thread_jobs.pop(thread_id, None)
            raise
        turn = result.get("turn") or {}
        turn_id = turn.get("id", "")
        if not turn_id:
            job.status = "failed"
            self.starting_thread_jobs.pop(thread_id, None)
            raise RuntimeError("Codex did not return a turn id")
        job.turn_id = turn_id
        job.status = "in_progress"
        self.turn_jobs[turn_id] = job.id
        self.starting_thread_jobs.pop(thread_id, None)
        return job

    def get_job(self, job_id: str) -> CodexJob | None:
        return self.jobs.get(job_id)

    async def answer_approval(
        self, job_id: str, approval_id: str, decision: str
    ) -> CodexJob:
        if decision not in APPROVAL_DECISIONS:
            raise ValueError("Unsupported approval decision")
        job = self.jobs.get(job_id)
        if not job or approval_id not in job.approvals:
            raise KeyError("Approval not found")
        approval = job.approvals.pop(approval_id)
        self.approval_jobs.pop(approval_id, None)
        await self._send({"id": approval.rpc_id, "result": {"decision": decision}})
        return job

    async def cancel_job(self, job_id: str) -> CodexJob:
        job = self.jobs.get(job_id)
        if not job:
            raise KeyError("Job not found")
        await self._request(
            "turn/interrupt", {"threadId": job.thread_id, "turnId": job.turn_id}
        )
        job.status = "interrupted"
        return job

    async def close(self) -> None:
        if self.process and self.process.returncode is None:
            self.process.terminate()
            try:
                await asyncio.wait_for(self.process.wait(), timeout=3)
            except TimeoutError:
                self.process.kill()
        for task in (self.reader_task, self.stderr_task):
            if task and not task.done():
                task.cancel()
