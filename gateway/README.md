# Cardputer Agent Gateway

The gateway accepts a raw WAV body at `POST /v1/voice-notes`, authenticates the
device, validates the audio, deduplicates it by SHA-256, transcribes it, and
atomically creates a Markdown note in a scoped Obsidian folder.

It also exposes a small authenticated HTTPS facade over the local Codex App
Server. The Cardputer can list/read existing threads, start text or transcribed
voice turns, poll accumulated answers, cancel work, and answer command/file
approvals without receiving ChatGPT OAuth tokens or an OpenAI API key.

## Run locally

```sh
python3 gateway/scripts/configure.py
cd gateway
scripts/generate-certificates.sh certs mac-mini.local 192.168.1.20
./run-gateway.sh
```

The configuration wizard reads Obsidian's macOS Vault registry when available,
lets you select a detected Vault or enter another path, generates a strong
device token, and writes a shell-safe `gateway/.env` with mode `0600`. Re-run it
whenever you want to change the Vault, destination folder, transcription
provider, Codex integration, or TTS. For unattended setup:

```sh
python3 gateway/scripts/configure.py \
  --non-interactive \
  --vault "/absolute/path/to/My Vault" \
  --notes-folder "Inbox/Cardputer Voice"
```

The backend reads `OBSIDIAN_VAULT_ROOT` and `OBSIDIAN_NOTES_FOLDER` at startup,
so restart it after editing `.env`. Personal paths and secrets never need to be
placed in firmware or committed to Git.

Replace the example host name and IP with addresses reachable from your
Cardputer. Copy `gateway/certs/cardputer-agent-ca.pem` to the SD-card root as
`AGENT_CA.PEM`. Public firmware embeds no developer trust anchor and refuses
HTTPS when neither that file nor the legacy `VOICE_CA.PEM` is present.

On macOS, after configuration and certificate generation, install the included
per-user LaunchAgent with `gateway/scripts/install-macos.sh`. The installer
renders the generic template with the current checkout and home directories;
the committed template contains no personal path.

For first testing use `TRANSCRIPTION_PROVIDER=mock`. For real transcription use
`whisper-server` with a loopback `WHISPER_SERVER_URL`, `openai` with a
server-side `OPENAI_API_KEY`, or install the `local-whisper` extra and select
`faster-whisper`. The `whisper-server` provider refuses non-loopback addresses,
so WAV data cannot accidentally leave the Mac. Codex App Server currently accepts
text/image user inputs, not raw WAV, so the gateway transcribes first and sends
the resulting text into the selected Codex thread.

Set `CODEX_ENABLED=true` to enable Codex routes. The gateway starts
`codex app-server --listen stdio://` lazily and uses the account already managed
by the local Codex installation. Keep the gateway on the same Mac as those
thread logs.

## Device API

- `POST /v1/voice/jobs` accepts audio and returns a durable job immediately
- `GET /v1/voice/jobs` lists processing history
- `GET /v1/voice/jobs/{jobId}` reports stage, progress, error, and results
- `POST /v1/voice/jobs/{jobId}/retry|cancel|reprocess`
- `POST /v1/voice/jobs/retry-failed` retries every failed job for this device
- `GET /v1/profiles`
- `POST /v1/voice` with `X-Voice-Destination: note|codex|both`
- `GET /v1/codex/chats`
- `GET /v1/codex/chats/{threadId}`
- `GET /v1/codex/chats/{threadId}/speech?scope=last|conversation`
- `POST /v1/codex/chats/{threadId}/messages`
- `GET /v1/codex/jobs/{jobId}`
- `POST /v1/codex/jobs/{jobId}/approvals/{approvalId}`
- `POST /v1/codex/jobs/{jobId}/cancel`

The old `POST /v1/voice-notes` endpoint remains compatible.

Voice jobs are stored in SQLite before processing and unfinished jobs resume
after a gateway restart. Audio is retained in the private spool so another
profile can reuse the existing transcript. Built-in profiles are `default`,
`meeting`, `idea`, and `task`; profile notes are routed below the configured
Obsidian Cardputer folder.

Long recordings may need more than the default request time of small cloud
transcription APIs. `TRANSCRIPTION_TIMEOUT_SECONDS=1800` gives the local
Whisper server up to 30 minutes while its durable job remains visible.

On macOS the gateway advertises `_cardputer-agent._tcp` through Bonjour while
it runs. Set `GATEWAY_ADVERTISE_MDNS=false` to disable this, or
`GATEWAY_ADVERTISE_PORT` when the HTTPS listener is not on port 8765.

Set `AGENT_BASE_URL=http://127.0.0.1:8642` to pass transcripts through a local
Hermes Gateway before writing the note. The endpoint is OpenAI-compatible, so
other compatible agent gateways work as well.

Set `TRANSCRIPT_FORMATTER=codex-exec` to edit transcripts through the locally
authenticated Codex CLI. The gateway uses an ephemeral, read-only
`codex exec` run with approvals disabled, passes the raw transcript over stdin,
and stores both edited Markdown and the raw transcript in the Obsidian note.

Set `OBSIDIAN_AUDIO_FORMAT=mp3` (the default) to convert each accepted WAV with
local FFmpeg into a 64 kbps MP3 beside its Markdown note. The note embeds that
file with an Obsidian `![[filename.mp3]]` link. Set `FFMPEG_EXECUTABLE` when
FFmpeg is not available on `PATH`, or use `OBSIDIAN_AUDIO_FORMAT=none` to
disable audio export.

Set `TTS_ENABLED=true` on macOS to let Cardputer play the latest Codex answer
or the visible conversation. The gateway uses the local `say` voice and FFmpeg
to return mono 16 kHz PCM WAV; the text and synthesized audio stay on the Mac.
Choose a Russian voice with `TTS_VOICE=Milena` and adjust `TTS_RATE` as needed.

`GET /v1/status` is an authenticated end-to-end diagnostic for Gateway,
WhisperServer, Codex App Server, and the configured transcript formatter.

Do not expose the HTTP development server directly to the public internet. For
remote use, place it behind an HTTPS reverse proxy and copy that proxy's CA
certificate to the Cardputer SD card as `/AGENT_CA.PEM`.
