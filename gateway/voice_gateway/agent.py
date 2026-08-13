from __future__ import annotations

import asyncio
import shutil

import httpx

from .config import Settings


class AgentFormatter:
    """Optional transcript editor backed by Codex CLI or an HTTP agent."""

    def __init__(self, settings: Settings):
        self.settings = settings
        self._codex_lock = asyncio.Lock()

    async def format(self, transcript: str, instruction: str = "") -> str | None:
        provider = self.settings.transcript_formatter
        if provider == "none":
            return None
        if provider == "codex-exec":
            return await self._codex_exec(transcript, instruction)
        if provider != "openai-compatible":
            raise RuntimeError(f"Unsupported transcript formatter: {provider}")
        if not self.settings.agent_base_url:
            raise RuntimeError("AGENT_BASE_URL is required for HTTP formatting")
        return await self._openai_compatible(transcript, instruction)

    async def status(self) -> dict[str, str]:
        provider = self.settings.transcript_formatter
        if provider == "none":
            return {"status": "disabled", "provider": provider}
        if provider == "codex-exec":
            executable = shutil.which(self.settings.codex_executable)
            return {
                "status": "ok" if executable else "error",
                "provider": provider,
                "detail": "ready" if executable else "Codex executable not found",
            }
        if provider == "openai-compatible":
            return {
                "status": "ok" if self.settings.agent_base_url else "error",
                "provider": provider,
                "detail": "configured" if self.settings.agent_base_url else "URL missing",
            }
        return {"status": "error", "provider": provider, "detail": "unsupported"}

    async def _codex_exec(self, transcript: str, instruction: str) -> str:
        executable = shutil.which(self.settings.codex_executable)
        if not executable:
            raise RuntimeError(
                f"Codex executable not found: {self.settings.codex_executable}"
            )
        prompt = (
            "Edit the Russian voice transcript provided on stdin into concise "
            "Obsidian Markdown. Correct recognition errors, spelling, punctuation, "
            "and broken sentences. Preserve every fact, decision, task, name, and "
            "uncertainty. Do not invent information. Return only the edited Markdown "
            "without commentary or a code fence. Do not run tools."
        )
        if instruction:
            prompt += " Profile instruction: " + instruction
        command = [
            executable,
            "--ask-for-approval",
            "never",
            "--sandbox",
            "read-only",
        ]
        if self.settings.codex_default_cwd:
            command.extend(["--cd", self.settings.codex_default_cwd])
        command.extend([
            "exec",
            "--ephemeral",
            "--skip-git-repo-check",
            "--ignore-rules",
            "--color",
            "never",
            prompt,
        ])
        async with self._codex_lock:
            process = await asyncio.create_subprocess_exec(
                *command,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            try:
                stdout, stderr = await asyncio.wait_for(
                    process.communicate(transcript.encode("utf-8")),
                    timeout=self.settings.codex_formatter_timeout_seconds,
                )
            except TimeoutError as error:
                process.kill()
                await process.wait()
                raise RuntimeError("Codex transcript formatting timed out") from error
        if process.returncode != 0:
            detail = stderr.decode("utf-8", "replace").strip()[-600:]
            raise RuntimeError(f"Codex transcript formatting failed: {detail}")
        formatted = stdout.decode("utf-8", "replace").strip()
        if not formatted:
            raise RuntimeError("Codex transcript formatter returned empty text")
        return formatted

    async def _openai_compatible(self, transcript: str, instruction: str) -> str:
        headers = {"Content-Type": "application/json"}
        if self.settings.agent_api_key:
            headers["Authorization"] = f"Bearer {self.settings.agent_api_key}"
        payload = {
            "model": self.settings.agent_model or None,
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "Turn the voice transcript into concise Obsidian Markdown. "
                        "Preserve facts, decisions, tasks, names, and uncertainty. "
                        "Return Markdown only and never invent missing details."
                    ) + (" " + instruction if instruction else ""),
                },
                {"role": "user", "content": transcript},
            ],
        }
        if payload["model"] is None:
            del payload["model"]
        async with httpx.AsyncClient(timeout=180) as client:
            response = await client.post(
                f"{self.settings.agent_base_url}/v1/chat/completions",
                headers=headers,
                json=payload,
            )
        response.raise_for_status()
        return response.json()["choices"][0]["message"]["content"].strip()
