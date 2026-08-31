from __future__ import annotations

import asyncio
import re
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

    async def title(self, transcript: str) -> str:
        fallback = self._fallback_title(transcript)
        provider = self.settings.transcript_formatter
        try:
            if provider == "codex-exec":
                result = await self._codex_title(transcript)
            elif provider == "openai-compatible" and self.settings.agent_base_url:
                result = await self._openai_title(transcript)
            else:
                return fallback
        except (RuntimeError, httpx.HTTPError, KeyError, IndexError, TypeError):
            return fallback
        return self._clean_title(result) or fallback

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
                "detail": "configured"
                if self.settings.agent_base_url
                else "URL missing",
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
        return await self._run_codex(transcript, prompt)

    async def _codex_title(self, transcript: str) -> str:
        prompt = (
            "Create one specific, human-readable Russian title for the voice "
            "transcript provided on stdin. Capture its main topic or requested "
            "action. Use 3-9 words and at most 72 characters. Do not use generic "
            "labels such as 'Заметка' or 'Запись'. Return only the title as plain "
            "text, without quotes, Markdown, a period, or commentary. Do not run "
            "tools and do not invent details."
        )
        return await self._run_codex(transcript, prompt)

    async def _run_codex(self, transcript: str, prompt: str) -> str:
        executable = shutil.which(self.settings.codex_executable)
        if not executable:
            raise RuntimeError(
                f"Codex executable not found: {self.settings.codex_executable}"
            )
        command = [
            executable,
            "--ask-for-approval",
            "never",
            "--sandbox",
            "read-only",
        ]
        if self.settings.codex_default_cwd:
            command.extend(["--cd", self.settings.codex_default_cwd])
        command.extend(
            [
                "exec",
                "--ephemeral",
                "--skip-git-repo-check",
                "--ignore-rules",
                "--color",
                "never",
                prompt,
            ]
        )
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
                if process.returncode is None:
                    try:
                        process.kill()
                    except OSError:
                        pass
                try:
                    await asyncio.wait_for(process.wait(), timeout=5)
                except (TimeoutError, OSError):
                    pass
                raise RuntimeError("Codex transcript formatting timed out") from error
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
                    )
                    + (" " + instruction if instruction else ""),
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
        return self._response_text(response, "Transcript formatter")

    async def _openai_title(self, transcript: str) -> str:
        headers = {"Content-Type": "application/json"}
        if self.settings.agent_api_key:
            headers["Authorization"] = f"Bearer {self.settings.agent_api_key}"
        payload = {
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "Create one specific title for this voice transcript in "
                        "the transcript's language. Use 3-9 words and no more than "
                        "72 characters. Return plain text only, without quotes or "
                        "punctuation at the end. Never invent details."
                    ),
                },
                {"role": "user", "content": transcript},
            ],
        }
        if self.settings.agent_model:
            payload["model"] = self.settings.agent_model
        async with httpx.AsyncClient(timeout=90) as client:
            response = await client.post(
                f"{self.settings.agent_base_url}/v1/chat/completions",
                headers=headers,
                json=payload,
            )
        response.raise_for_status()
        return self._response_text(response, "Title formatter")

    @staticmethod
    def _response_text(response: httpx.Response, provider: str) -> str:
        """Extract a usable chat completion instead of leaking KeyError/TypeError."""
        try:
            payload = response.json()
        except (ValueError, UnicodeError, TypeError) as error:
            raise RuntimeError(f"{provider} returned invalid JSON") from error
        if not isinstance(payload, dict):
            raise RuntimeError(f"{provider} returned an invalid response")
        choices = payload.get("choices")
        if not isinstance(choices, list) or not choices:
            raise RuntimeError(f"{provider} returned no choices")
        first = choices[0]
        if not isinstance(first, dict):
            raise RuntimeError(f"{provider} returned an invalid choice")
        message = first.get("message")
        if not isinstance(message, dict):
            raise RuntimeError(f"{provider} returned an invalid message")
        content = message.get("content")
        if not isinstance(content, str) or not content.strip():
            raise RuntimeError(f"{provider} returned empty text")
        return content.strip()

    @classmethod
    def _fallback_title(cls, transcript: str) -> str:
        text = re.sub(r"\s+", " ", transcript).strip()
        text = re.sub(r"^так[,.:;!]?\s+", "", text, flags=re.IGNORECASE)
        text = re.sub(
            r"^(?:пожалуйста[,.:;!]?\s+)?"
            r"(?:запиши|записать|создай заметку|заметка)\s+",
            "",
            text,
            flags=re.IGNORECASE,
        )
        sentence = re.split(r"(?<=[.!?])\s+", text, maxsplit=1)[0]
        if len(sentence) > 72:
            sentence = sentence[:73].rsplit(" ", 1)[0]
        return cls._clean_title(sentence) or "Голосовая заметка"

    @staticmethod
    def _clean_title(value: str) -> str:
        value = value.splitlines()[0] if value else ""
        value = re.sub(r"^\s*#{1,6}\s*", "", value)
        value = value.strip().strip("`'\"«»“”")
        value = re.sub(r"\s+", " ", value)
        value = re.sub(r"[.!?,:;\-–—]+$", "", value).strip()
        return value[:72].strip()
