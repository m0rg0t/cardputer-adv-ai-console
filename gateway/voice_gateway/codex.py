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
            # A previous app-server may have exited (or failed during
            # initialization) while its reader tasks are still around.  Shut
            # it down completely before replacing ``self.process``; the
            # reader uses that attribute while it drains stdout.
            if self.process or self.reader_task or self.stderr_task:
                await self._shutdown_process("Codex App Server restarting")
            self.stderr_tail = ""
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
            try:
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
            except BaseException:
                # If initialize/initialized fails, do not leave a half-ready
                # process behind.  Otherwise the next request sees a live
                # process and skips initialization forever.
                await self._shutdown_process("Codex App Server initialization failed")
                raise

    async def _shutdown_process(self, reason: str) -> None:
        """Stop the app-server and drain its tasks without leaking work."""
        process = self.process
        tasks = (self.reader_task, self.stderr_task)
        if process and process.returncode is None:
            try:
                process.terminate()
            except ProcessLookupError:
                pass
            except OSError:
                pass
        if process:
            try:
                await asyncio.wait_for(process.wait(), timeout=3)
            except TimeoutError:
                if process.returncode is None:
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                    except OSError:
                        pass
                try:
                    await asyncio.wait_for(process.wait(), timeout=3)
                except (TimeoutError, ProcessLookupError, OSError):
                    pass
            except (ProcessLookupError, OSError):
                pass

        current = asyncio.current_task()
        for task in tasks:
            if task and task is not current and not task.done():
                task.cancel()
        pending_tasks = [
            task for task in tasks if task and task is not current
        ]
        if pending_tasks:
            await asyncio.gather(*pending_tasks, return_exceptions=True)

        for future in self.pending.values():
            if not future.done():
                future.set_exception(RuntimeError(reason))
        self.pending.clear()
        self.starting_thread_jobs.clear()
        self.turn_jobs.clear()
        self.approval_jobs.clear()
        self._mark_jobs_failed(reason)

        if self.process is process:
            self.process = None
            self.reader_task = None
            self.stderr_task = None

    def _mark_jobs_failed(self, reason: str) -> None:
        for job in self.jobs.values():
            if job.status in {"queued", "starting", "in_progress"}:
                job.status = "failed"
                job.error = reason

    async def _read_stderr(self) -> None:
        assert self.process and self.process.stderr
        while line := await self.process.stderr.readline():
            self.stderr_tail = line.decode("utf-8", "replace").strip()[-1000:]

    async def _read_stdout(self) -> None:
        assert self.process and self.process.stdout
        while line := await self.process.stdout.readline():
            try:
                message = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue
            if not isinstance(message, dict):
                # A broken app-server line must not kill the reader task.  The
                # next valid response may still complete an in-flight request.
                continue
            if "id" in message and "method" not in message:
                request_id = message.get("id")
                if not isinstance(request_id, (int, str)):
                    continue
                future = self.pending.pop(request_id, None)
                if future and not future.done():
                    if "error" in message:
                        future.set_exception(
                            RuntimeError(self._error_detail(message.get("error")))
                        )
                    else:
                        result = message.get("result", {})
                        if not isinstance(result, dict):
                            future.set_exception(
                                RuntimeError("Codex returned an invalid response")
                            )
                        else:
                            future.set_result(result)
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
        self.starting_thread_jobs.clear()
        self.turn_jobs.clear()
        self.approval_jobs.clear()
        self._mark_jobs_failed(detail)

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
        try:
            await self._send({"method": method, "id": request_id, "params": params})
            return await asyncio.wait_for(future, timeout=30)
        finally:
            # A timeout or task cancellation otherwise leaves a future in the
            # pending map until the app-server eventually sends a response.
            self.pending.pop(request_id, None)

    async def _notify(self, method: str, params: dict[str, Any]) -> None:
        await self._send({"method": method, "params": params})

    @staticmethod
    def _error_detail(error: Any) -> str:
        if isinstance(error, dict):
            detail = error.get("message") or error.get("detail") or error.get("code")
            if isinstance(detail, (str, int, float)) and str(detail).strip():
                return str(detail).strip()
            return "Codex returned an invalid error"
        if isinstance(error, str) and error.strip():
            return error.strip()
        return "Codex returned an invalid error"

    @staticmethod
    def _text(value: Any) -> str:
        return value if isinstance(value, str) else ""

    def _receive_notification(self, message: dict[str, Any]) -> None:
        method = message.get("method")
        if not isinstance(method, str):
            return
        params = message.get("params")
        if not isinstance(params, dict):
            return
        turn = params.get("turn")
        turn_data = turn if isinstance(turn, dict) else {}
        turn_id = self._text(params.get("turnId")) or self._text(turn_data.get("id"))
        if method == "turn/started" and turn_id:
            thread_id = self._text(params.get("threadId")) or self._text(
                turn_data.get("threadId")
            )
            starting_job_id = self.starting_thread_jobs.get(thread_id)
            if not starting_job_id and len(self.starting_thread_jobs) == 1:
                starting_job_id = next(iter(self.starting_thread_jobs.values()))
            if starting_job_id:
                starting_job = self.jobs.get(starting_job_id)
                if not starting_job:
                    return
                starting_job.turn_id = turn_id
                self.turn_jobs[turn_id] = starting_job_id
        job_id = self.turn_jobs.get(turn_id or "")
        if not job_id:
            return
        job = self.jobs.get(job_id)
        if not job:
            return
        if method == "item/agentMessage/delta":
            delta = params.get("delta", "")
            if isinstance(delta, str):
                job.text += delta
        elif method == "turn/completed":
            status = turn_data.get("status", "completed")
            if not isinstance(status, str) or not status:
                status = "failed"
            job.status = "completed" if status == "completed" else status
            error = turn_data.get("error")
            if isinstance(error, dict):
                job.error = self._text(error.get("message")) or self._text(
                    error.get("detail")
                )
            elif isinstance(error, str):
                job.error = error
            else:
                job.error = ""

    def _receive_approval(self, message: dict[str, Any]) -> None:
        params = message.get("params")
        if not isinstance(params, dict):
            return
        turn_id = self._text(params.get("turnId"))
        job_id = self.turn_jobs.get(turn_id)
        if not job_id:
            return
        rpc_id = message.get("id")
        method = message.get("method")
        if not isinstance(rpc_id, (int, str)) or not isinstance(method, str):
            return
        approval_id = uuid.uuid4().hex
        command = params.get("command") or ""
        if isinstance(command, list):
            command = " ".join(str(part) for part in command)
        elif not isinstance(command, str):
            command = str(command)
        approval = CodexApproval(
            id=approval_id,
            rpc_id=rpc_id,
            method=method,
            thread_id=self._text(params.get("threadId")),
            turn_id=turn_id,
            reason=self._text(params.get("reason")),
            command=command,
            cwd=self._text(params.get("cwd")) or self._text(params.get("grantRoot")),
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
        threads = result.get("data", [])
        if not isinstance(threads, list) or any(
            not isinstance(thread, dict) for thread in threads
        ):
            raise RuntimeError("Codex returned an invalid thread list")
        return threads

    async def read_thread(self, thread_id: str) -> dict[str, Any]:
        await self._ensure_started()
        result = await self._request(
            "thread/read", {"threadId": thread_id, "includeTurns": False}
        )
        thread = result.get("thread", result)
        if not isinstance(thread, dict):
            raise RuntimeError("Codex returned an invalid thread")
        turns_result = await self._request(
            "thread/turns/list",
            {
                "threadId": thread_id,
                "limit": 8,
                "sortDirection": "desc",
                "itemsView": "summary",
            },
        )
        turns = turns_result.get("data", [])
        if not isinstance(turns, list) or any(
            not isinstance(turn, dict) for turn in turns
        ):
            raise RuntimeError("Codex returned invalid thread turns")
        thread["turns"] = list(reversed(turns))
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
        thread = result.get("thread", result)
        if not isinstance(thread, dict):
            raise RuntimeError("Codex returned an invalid thread")
        return thread

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
        except asyncio.CancelledError:
            job.status = "failed"
            job.error = "Codex turn start canceled"
            self.starting_thread_jobs.pop(thread_id, None)
            raise
        except Exception:
            job.status = "failed"
            self.starting_thread_jobs.pop(thread_id, None)
            raise
        turn = result.get("turn")
        if not isinstance(turn, dict):
            job.status = "failed"
            self.starting_thread_jobs.pop(thread_id, None)
            raise RuntimeError("Codex did not return a valid turn")
        turn_id = turn.get("id")
        if not isinstance(turn_id, str) or not turn_id:
            job.status = "failed"
            self.starting_thread_jobs.pop(thread_id, None)
            raise RuntimeError("Codex did not return a turn id")
        job.turn_id = turn_id
        self.turn_jobs[turn_id] = job.id
        self.starting_thread_jobs.pop(thread_id, None)
        if job.status == "interrupted":
            # The device may cancel while ``turn/start`` is still pending.  A
            # turn id is available now, so finish delivering that interrupt;
            # keep the local job interrupted even if the server has already
            # started streaming deltas.
            try:
                await self._request(
                    "turn/interrupt", {"threadId": thread_id, "turnId": turn_id}
                )
            except RuntimeError as error:
                job.error = str(error)[:300]
            return job
        job.status = "in_progress"
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
        if job.status in {"completed", "failed", "interrupted"}:
            return job
        if not job.turn_id:
            # ``turn/start`` may still be waiting for the server response.  A
            # later response is handled by start_turn, which sends the actual
            # interrupt once it receives the turn id.
            job.status = "interrupted"
            job.error = "Canceled before turn started"
            self.starting_thread_jobs.pop(job.thread_id, None)
            return job
        await self._request(
            "turn/interrupt", {"threadId": job.thread_id, "turnId": job.turn_id}
        )
        job.status = "interrupted"
        return job

    async def close(self) -> None:
        await self._shutdown_process("Codex App Server stopped")
