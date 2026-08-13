# Agent Console architecture

## Boundary

The firmware is one M5Apps application with two explicit product modes:

- **Notes** records durable audio and produces an Obsidian note.
- **Codex** sends typed or transcribed text to one selected Codex thread.
- **Both** performs both deliveries from one audio file and one transcript.

They share hardware and transport code, but a note is never implicitly treated
as an agent command.

## Data flow

```text
Cardputer ADV
  WAV + .ROUTE intent + .AGENT.JSON result
       |
       | authenticated HTTPS, retryable
       v
Cardputer Agent Gateway
  validate -> SHA-256 dedupe -> transcribe once
       |                         |
       | NOTE/BOTH               | CODEX/BOTH
       v                         v
Obsidian Markdown          Codex App Server
                           existing thread + streamed job
```

The `.ROUTE` sidecar stores the destination and exact thread id at record time.
Changing the selected chat later cannot redirect queued audio.
After delivery, `.AGENT.JSON` stores the result, Obsidian path, and a bounded
transcript preview so the library can show queued/delivered/transcribed state
and the user can inspect text offline. The complete transcript remains on the
Mac in Obsidian and the gateway database.

## Why transcription is in the gateway

Codex Desktop has a built-in voice user experience, but the documented Codex
App Server protocol exposes text, image, and local-image user inputs rather than
raw audio. The gateway therefore uses either OpenAI file transcription or local
`faster-whisper`, then sends the returned text through `turn/start`.

- Codex App Server: <https://developers.openai.com/codex/app-server>
- OpenAI file transcription: <https://developers.openai.com/api/docs/guides/speech-to-text>

Provider credentials remain on the gateway. The SD card contains only Wi-Fi,
the HTTPS base URL, a trusted CA certificate, and a revocable device token.

## Failure behavior

- No network: WAV and sidecar remain on SD for retry.
- Upload acknowledgement lost: gateway route-key deduplication returns the
  previous result.
- Transcription unavailable: delivery is not recorded as complete and retries.
- Codex unavailable: note delivery can still succeed for `NOTE`; `BOTH` retains
  a failure so the combined intent is not silently reduced.
- Approval required: the job remains visible with a command/file approval card.

## Cardputer controls

- `R` in Library: note recording.
- `C`: chat list.
- `T`: typed Codex message.
- `R` in Codex: voice to Codex.
- `B` in Codex: note plus Codex.
- `L` in Codex: return directly to Library.
- `I` in Library: recording status and transcript details.
- `Enter`, `S`, `D`, `X`: approve once, approve for session, decline, cancel.
