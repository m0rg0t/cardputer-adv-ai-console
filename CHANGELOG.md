# Changelog

## Unreleased

- Fix the web panel silently ignoring brightness, low-battery, seek-step, and
  sort changes: browser forms post numbers as strings, which the firmware now
  accepts alongside JSON numbers.
- Serve recordings with HTTP byte-range support so Safari/iOS can play them and
  every browser can seek.
- Refresh the web panel: status badges per recording, modified dates, storage
  and battery bars, file count with truncation notice, manual refresh, and a
  compact mobile layout.
- Bound voice-job polling on the device main loop to a 10 s read timeout so a
  slow gateway cannot freeze the UI for 30 s per poll.
- Use 64-bit arithmetic in WAV chunk bounds checks.
- Gateway: reject unknown voice profiles with 400 instead of silently using
  `default`, record the Codex turn id before honouring cancellation, split long
  WAVs for whisper-server in a worker thread, and prune finished Codex jobs
  from memory.
- Add a compact same-LAN recorder panel at `recorder.local` with live device,
  Wi-Fi, battery, and microSD status, WAV playback/download, and persistent
  recorder settings.
- Show live WAV upload percentage and a full-width progress bar in the device
  header while a background transfer is active.
- Split long local-Whisper WAV transcriptions into ordered 10-minute requests,
  retry transiently failed chunks, and remove temporary audio after processing.
- Track the current M5Apps `0x170000` app partition in release-size checks;
  older project checks still assumed `0x160000`.
- Add compact and full wrapped Codex task-name modes, persisted under Reading
  settings.
- Add a one-key flow that creates a local Codex task and records its first
  voice request.
- Add ElevenLabs speech synthesis with named-voice resolution, stable voice-ID
  configuration, WAV caching/conversion, Keychain loading, and a configured
  Jarvis-style preset.
- Generate transcript-derived titles for Obsidian notes and safely rename
  completed Cardputer WAVs and route metadata with the same dated
  human-readable title.

## 2.8.1 - Selectable screen saver style

- Add a lightweight animated Data Rain screen saver alongside Cyber Grid.
- Keep recording, playback, battery, and triple-press wake information visible
  over either procedural animation without bitmap assets.
- Add `Settings → Screen Saver → Visual Style` and persist the selection as
  `screen_style` in `RECORDER.CFG`.

## 2.8.0 - Searchable recording library

- Add persistent library sorting by newest, oldest, processing status, or
  filename through both the `S` shortcut and Settings.
- Keep the selected recording stable while order changes and automatically
  regroup the status view as background processing advances.
- Show the active sort mode in the library header and document the controls in
  contextual Help and the SD-card configuration reference.

## 2.7.2 - Public repository packaging

- Split firmware source, local gateway, documentation, and ready-to-install
  binaries into explicit top-level directories.
- Load a user-provided `/AGENT_CA.PEM` or legacy `/VOICE_CA.PEM` from microSD;
  public firmware no longer embeds a developer-specific trust anchor.
- Add a private gateway configuration wizard with Obsidian Vault discovery,
  strong token generation, safe shell quoting, and unattended setup support.
- Add a generic macOS LaunchAgent template and installer without personal paths.
- Exclude recordings, transcripts, partition backups, third-party firmware,
  certificates, databases, and local settings from publication.

## 2.7.1 - Complete contextual help

- Expand on-device help from six to ten pages covering files, recording,
  playback, Outbox, Codex tasks/chat, recording details, and settings.
- Add Help to the main Settings menu and open the matching page with `H` from
  supported modes, returning to the original screen on close.

## 2.7.0 - Responsive transfers and accessible reading

- Move long WAV uploads off the UI loop so screens and keys remain responsive.
- Retry every failed transcription from Outbox with `A`, and keep failed local
  sidecars synchronized when a retry is initiated from the Mac gateway.
- Add independent 1x/2x font settings for Codex chat and transcripts.
- Add `E` jump-to-end plus local macOS speech for the latest Codex reply or the
  conversation, played as a temporary WAV on Cardputer.
- Add a lightweight animated cyberpunk screen saver without bitmap assets.
- Raise the documented gateway upload limit to 128 MiB for long recordings.

## 2.6.4 - Strict ESP32 certificate chain

- Replace the local certificate chain with an ESP32-compatible RSA-2048 CA
  carrying explicit critical `CA:TRUE` and `keyCertSign` extensions.
- Add a reproducible certificate generator and strict X.509 verification.
- Include the gateway IPv4 address in both IP and DNS SAN forms for the legacy
  hostname verifier shipped by Arduino-ESP32 2.x.

## 2.6.3 - Deterministic Gateway trust

- Trust only the bundled current local Gateway CA for the personal Mac mini
  endpoint, so a stale certificate left on SD cannot enter mbedTLS validation.
- Log the exact HTTPS URL and synchronized epoch before each TLS connection.

## 2.6.2 - Gateway TLS trust order

- Load the current bundled local Gateway CA before an optional SD certificate,
  preventing a stale `/AGENT_CA.PEM` from winning trust-anchor selection.
- Stop probing `.AGENT.JSON` sidecars for directories and unrelated SD files.

## 2.6.1 - M5Apps slot-size fix

- Replace the heavy ESPmDNS dependency with a compact built-in DNS-SD query,
  keeping local gateway discovery while fitting the 1408 KiB M5Apps slot.
- Make release packaging fail when a firmware image exceeds the real
  Cardputer ADV Agent Console partition size (`0x160000`).

## 2.6.0 - Reliable voice queue and full Agent workflow

- Accept recordings into a persistent asynchronous gateway queue and expose
  transcription, formatting, Obsidian, and Codex progress independently.
- Add the on-device Outbox with progress, detailed errors, retry, cancel, and
  restart-safe job IDs stored beside each WAV.
- Add `default`, `meeting`, `idea`, and `task` processing profiles; the selected
  profile travels with the recording route and controls note structure/folder.
- Add recording pause/resume while keeping the live level meter and accurate
  recorded duration.
- Add Codex task creation and interruption, plus sending a cached transcript to
  a chosen existing task without running Whisper again.
- Add reprocessing with another profile using the stored transcript, retaining
  gateway processing history and avoiding duplicate transcription.
- Add optional Bonjour/mDNS gateway discovery with an explicit HTTPS fallback.
- Add optional 8 kHz compact PCM recording for half-size, low-bandwidth WAVs
  while retaining the interoperable 16 kHz default.
- Add a persistent header strip for Wi-Fi, gateway HTTP health, and queue size.

## 2.5.0 - End-to-end services and Codex transcript editing

- Add an authenticated Services screen showing real Gateway, WhisperServer,
  Codex App Server, Codex transcript editor, and pending-recording states.
- Add `/v1/status` to exercise the configured local dependencies instead of
  inferring service health from Wi-Fi association alone.
- Edit Whisper transcripts with sandboxed, ephemeral `codex exec` before
  writing the polished Markdown and retain the raw transcript below it.
- Bundle the current public local CA as a fallback trust anchor while still
  accepting `/AGENT_CA.PEM` from SD, recovering from a stale card certificate.

## 2.4.0 - Gateway and transcription visibility

- Synchronize the ESP32 clock over NTP before verified HTTPS requests, fixing
  X.509 failures on devices whose system clock has not yet been initialized.
- Show the concrete Gateway operation, HTTP code, transport failure, or TLS
  setup problem on Codex and recording detail screens.
- Distinguish queued, currently processing, failed, delivered, transcribed,
  and Obsidian-saved recordings in the library and detail view.
- Clear stale Wi-Fi disconnect reasons after DHCP succeeds and show live RSSI
  and channel for the current connection.

## 2.3.3 - M5Apps NVS compatibility

- Run the ESP32 Wi-Fi driver without its internal NVS persistence when launched
  under M5Apps, whose shared partition is named `apps_nvs` rather than the
  Arduino default `nvs`.
- Keep using the normal Arduino Wi-Fi, event, DHCP, and socket layers while all
  credentials remain in `AGENT.CFG` and `AGENT_WIFI.CFG` on microSD.
- Avoid erasing, relabeling, or otherwise modifying the M5Apps partition table
  and shared settings.

## 2.3.2 - Explicit Wi-Fi driver initialization

- Force a clean `WIFI_OFF` to `WIFI_STA` transition when Agent Console starts,
  so the Arduino layer receives a fresh station-start event under M5Apps.
- Wait for station startup before connecting or scanning.
- Fully restart the station driver before the one permitted scan retry.
- Include mode-start, raw Wi-Fi mode, and `WL_*` status in scan diagnostics.

## 2.3.1 - Reliable Wi-Fi scanning

- Cancel an unfinished station association before starting an on-device scan;
  ESP-IDF otherwise rejects the scan while the radio is still connecting.
- Clear stale scan state and retry once after resetting the station state.
- Show the scan result and Wi-Fi status codes instead of a generic failure.

## 2.3.0 - Wi-Fi scanner and connection diagnostics

- Add an on-device scan list with SSID, RSSI, channel, and security mode.
- Let the user select any visible network, enter its password, and connect
  without editing the card on another computer.
- Persist only successfully connected ad-hoc networks in `/AGENT_WIFI.CFG` and
  include them in the normal ordered retry cycle on future boots.
- Show the last ESP32 disconnect reason and whether the active SSID was visible
  in the latest scan, including RSSI, channel, and security mode.
- Retain the original five `AGENT.CFG` profiles and allow up to five additional
  networks saved by the device.

## 2.2.0 - Ordered Wi-Fi fallback profiles

- Read up to five Wi-Fi SSID/password pairs from `AGENT.CFG` while preserving
  the original unnumbered keys as profile 1.
- Try configured networks in order, pause after a failed cycle, and repeat.
- Show the active profile number and actual connected SSID on the Network page.
- Make manual Network retry restart the sequence from profile 1.

## 2.1.0 - Recording status and network visibility

- Add a Network settings page with Wi-Fi state, SSID, local IP, gateway URL,
  last HTTP status, and a manual reconnect action.
- Add `L` as a direct shortcut from Codex chats/conversations to the recorder.
- Mark library recordings as queued, delivered, or transcribed.
- Add a recording details screen with delivery destination and the transcript
  preview cached on microSD.
- Preserve transcript metadata when renaming recordings and remove it when the
  matching WAV is deleted.
- Let the gateway return a bounded transcript preview while retaining the full
  transcript in the personal Obsidian vault and gateway database.

## 2.0.0 - Agent Console

- Combine the recorder and Codex remote client in one M5Apps application while
  keeping Notes and Codex as explicit destinations.
- Add `NOTE`, `CODEX`, and `BOTH` voice delivery with per-WAV `.ROUTE` sidecars.
- Add an authenticated gateway facade over the local Codex App Server.
- List/read existing Codex chats, send keyboard or transcribed voice messages,
  poll answers, cancel work, and answer command/file approvals.
- Add UTF-8-safe wrapping and a Unicode M5GFX font for multilingual chat text.
- Add `/AGENT.CFG` and `/AGENT_CA.PEM` while retaining legacy configuration
  filenames.
- Package releases as app-only binaries for M5Apps Installer instead of merged
  full-flash images.
- Add OpenAI and local faster-whisper transcription choices, Obsidian delivery,
  route-key deduplication, and compact responses for ESP32 memory safety.

## 1.4.0 - Power, settings, and playback speed

- Remove the unused `Idle Sleep WIP` setting.
- Add `Triple-Press Wake` as an optional Screen Saver wake guard.
- Add configurable low battery auto-save for active recordings.
- Simplify the Help menu and remove visible WIP settings.
- Add settings reset confirmation and show the current firmware version in
  settings.
- Add live playback speed control with `[` and `]`.
- Add a configurable playback seek step with a 5-second default.

## 1.3.0 - Playback controls and file management

- Add pause and resume during playback.
- Add 10-second seek backward and forward during playback.
- Keep volume controls on Up/Down and use `Esc` to stop playback.
- Show `PAUSED` in the playback UI and dimmed playback screen.
- Keep wake-first-key behavior for dimmed or black screen playback.
- Add library file rename.
- Add recording lock state with delete protection.
- Add delete confirmation for unlocked recordings.
- Sort recordings by filename descending.
- Add a paged help screen opened with `H` from the library.

## 1.2.0 - App structure and release packaging

- Split the recorder application implementation into focused app modules for
  recording flow, playback flow, capture buffering, UI, settings, screen saver,
  and file browsing.
- Keep `RecorderApp` as the public application entry point while reducing the
  main implementation file size.
- Simplify release packaging to publish one complete flash image plus
  `SHA256SUMS.txt`.
- Document complete release image flashing at offset `0x0000`.

## 1.1.0 - Display settings and screen saver

- Change the header label to `RECORDER`.
- Show SD free space and selected-file size/duration in the library.
- Add persistent settings with brightness control.
- Add screen saver settings for home, recording, and playback states.
- Add Dimmed Standby and Black screen saver modes that keep recording and
  playback running.
- Add wake-first-key handling so the first input from a dimmed or black screen
  only restores the display.
- Mark stored but not-yet-implemented settings as WIP in the menu.

## 1.0.0 - Initial release

- Initial standalone release of Cardputer ADV Recorder.
- Stream 16 kHz mono PCM recordings to microSD.
- Browse, play, adjust volume, and delete recordings on the device.
- Add native WAV parsing and recording-name tests.
- Add automated firmware builds, tests, and release artifacts.
