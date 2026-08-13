# Cardputer Agent Console code audit — 2026-08-11

## Fixed in 2.7.0

- **Long upload blocked the UI.** `HTTPClient::sendRequest()` streamed an entire
  WAV from the Arduino loop. Upload now runs on a dedicated FreeRTOS task; the
  library, keyboard, status header, and screen saver continue to update.
- **Long files were rejected.** The gateway limit was 32 MiB, below a 30-minute
  16 kHz PCM recording. The personal deployment and example now allow 128 MiB.
- **Long transcription could time out.** The Whisper request timeout is now
  configurable and is 1800 seconds in the personal deployment.
- **Externally retried jobs stayed red on Cardputer.** Failed sidecars are now
  polled periodically, and Outbox can retry all failed jobs for the device.
- **Reading accessibility.** Codex and transcript viewers have independent 1x
  and 2x settings; Codex also has jump-to-end and local text-to-speech.

## Recommended next changes

1. **Unify all firmware network calls behind one asynchronous command worker.**
   Automatic WAV upload is now backgrounded, but user-triggered chat refresh,
   service checks, approvals, and Wi-Fi connection attempts can still block for
   their shorter HTTP timeout. A single serialized network queue would remove
   this last source of UI stalls and prevent concurrent `HTTPClient` use.
2. **Add resumable/chunked uploads.** The current background transfer keeps the
   UI alive but reserves SD/network I/O until it finishes, so a new recording is
   intentionally refused during transfer. Chunk checkpoints would let recording
   pre-empt an upload and resume it afterward.
3. **Separate gateway stages into bounded workers.** The durable queue is safe,
   but one long Whisper/Codex job currently delays every later voice job. Use a
   small transcription semaphore and independent format/export workers.
4. **Add retention and disk quotas.** Source WAVs and TTS cache are intentionally
   private and reusable, but completed spool audio needs age/size pruning with a
   protected minimum history.
5. **Reduce ESP32 heap fragmentation.** Large `String` and `JsonDocument`
   allocations for 6000-character conversations should become paginated or
   streamed responses.
6. **Preserve firmware headroom.** The public 2.7.2 M5Apps image is 1,437,808
   bytes, leaving only 3,984 bytes in the `0x160000` slot. New substantial
   features should move server-side or replace existing strings/code paths.
7. **Expand device-side tests.** Native WAV tests and gateway integration tests
   are green; state transitions, retry synchronization, and background-task/SD
   exclusion still need hardware-in-loop coverage.

## Security notes

- HTTPS certificate validation and the gateway token remain mandatory.
- Wi-Fi credentials and the device token are portable plaintext SD settings;
  this is acceptable for the personal physical-device model, but a lost card
  should be treated as a compromised device and its token rotated.
- Codex, Whisper, MP3 conversion, and speech synthesis stay on the Mac mini.
