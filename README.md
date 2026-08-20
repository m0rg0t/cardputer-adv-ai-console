# Cardputer ADV Agent Console

A combined voice-note recorder and remote Codex client for the M5Stack
Cardputer ADV. Notes and Codex messages share recording, Wi-Fi, HTTPS, and
offline delivery infrastructure, but remain explicit destinations (`NOTE`,
`CODEX`, or `BOTH`). It is packaged as an app-only binary so it can live
alongside other applications managed by M5Apps.

The firmware records 16 kHz, mono, 16-bit PCM WAV files directly to microSD.
Recording and playback are streamed through fixed-size buffers, so file length
is not limited by available RAM.

> Only Cardputer ADV is supported. The original Cardputer uses different audio
> hardware and is not compatible with this firmware.

## Repository contents

| Directory | Contents |
| --- | --- |
| [`firmware/`](firmware/) | Complete PlatformIO source and SD-card templates |
| [`gateway/`](gateway/) | Local Python backend for Whisper, Codex, TTS, and Obsidian |
| [`release/firmware/`](release/firmware/) | Verified, ready-to-install M5Apps binary |
| [`docs/`](docs/) | Architecture, operation, troubleshooting, and hardware QA |
| [`site/`](site/) | Project website, release download, and Sites deployment source |

Private recordings, transcripts, credentials, certificates, gateway databases,
device backups, and third-party firmware are deliberately not part of the
repository. See [SECURITY.md](SECURITY.md) before publishing or deploying it.

## Screenshots

<p align="center">
  <img src="docs/images/record.jpg" alt="Record screen" width="32%">
  <img src="docs/images/play.jpg" alt="Play screen" width="32%">
  <img src="docs/images/library.jpg" alt="Library screen" width="32%">
</p>

<p align="center">
  <img src="docs/images/setting.jpg" alt="Settings screen" width="32%">
  <img src="docs/images/help.jpg" alt="Help screen" width="32%">
  <img src="docs/images/standby.jpg" alt="Standby screen" width="32%">
</p>

## Features

- Stream recordings to microSD as `/REC0001.WAV` through `/REC9999.WAV`.
- Browse, play, rename, lock, and delete WAV files from the device.
- Pause, seek, and adjust playback volume while listening.
- Show recording level, elapsed time, file size, battery status, SD free
  space, and selected-file details.
- Configure display brightness and screen saver behavior from the device.
- Keep recording or playback running while the screen is dimmed or off.
- Auto-save an active recording at the configured low battery threshold.
- Recover from missing cards, low space, and storage errors.
- Finalize WAV headers and sync storage before presenting a recording as saved.
- Queue recordings offline and upload them when configured Wi-Fi returns.
- Accept uploads into a restart-safe asynchronous gateway queue and show the
  exact transcription/formatting/Obsidian/Codex stage on the device.
- Generate a concise title after transcription, save Obsidian Markdown and MP3
  with that title plus date/time, and rename the delivered WAV and its sidecars
  on the Cardputer to the same human-readable title.
- Manage pending work in an Outbox with details, retry, and cancel actions.
- Authenticate to a configurable gateway without storing transcription-provider
  credentials on the device.
- Require certificate validation for HTTPS uploads.
- Store recorder, Wi-Fi, endpoint, certificate, and delivery state on microSD.
- Serve a local administration panel at `http://recorder.local/` with device,
  Wi-Fi, battery, and microSD status; WAV playback/download; and persistent
  recorder settings.
- List and open existing local Codex Desktop/CLI chats through a gateway.
- Switch the task list between compact one-line names and full wrapped names.
- Type messages on the Cardputer keyboard or send a recording to a selected
  Codex chat after server-side transcription.
- Poll streamed Codex work, show the accumulated answer, and approve or decline
  command/file requests from the device.
- Create and interrupt Codex tasks, and send an already cached transcript to a
  selected task without transcribing the audio again.
- Create a new local Codex task and immediately record its first voice prompt.
- Speak Codex replies with local macOS voices or an ElevenLabs voice selected
  by stable voice ID, while keeping provider credentials off the device.
- Route a recording to Obsidian, Codex, or both without duplicating audio or
  transcription work.
- Persist an exact route sidecar beside every WAV so delayed uploads cannot be
  redirected by a later chat selection.
- Show queued, delivered, and transcribed state in the recording library and
  keep a transcript preview on SD for the recording details screen.
- Show Wi-Fi state, active profile, SSID, local IP, last disconnect reason, and
  the selected network's latest radio observation on the Network settings page.
- Try up to five ordered Wi-Fi profiles from `AGENT.CFG` and show which profile
  supplied the active connection.
- Scan visible Wi-Fi networks on-device, including RSSI, channel, and security,
  then select a network and enter its password from the Cardputer keyboard.
- Save successfully connected ad-hoc networks to `/AGENT_WIFI.CFG` on microSD
  so they are tried on later boots without changing the main gateway config.
- Apply `default`, `meeting`, `idea`, or `task` profiles to recordings and
  reprocess a finished transcript under another profile.
- Optionally discover the local gateway through Bonjour/mDNS, with a pinned
  HTTPS URL as fallback.
- Pause/resume capture and optionally record 8 kHz compact PCM WAVs for half
  the SD and upload size.
- Keep `W`, `G`, and `Q` Wi-Fi/gateway/queue indicators visible in the header.

## Requirements

- M5Stack Cardputer ADV
- Writable FAT32 microSD card
- PlatformIO Core 6.1.19 or PlatformIO IDE

## Build and install with M5Apps

```sh
platformio run -d firmware -e cardputer-adv-recorder
```

Copy `firmware/.pio/build/cardputer-adv-recorder/firmware.bin` to the microSD card, then
select it in **M5Apps > Installer > SD**. This is the application binary; do not
flash a merged/full image at offset `0x0000`, because that would replace M5Apps.

You can also use the checked release image in
[`release/firmware/`](release/firmware/). The serial monitor runs at 115200
baud. See [`firmware/sdcard/README.md`](firmware/sdcard/README.md)
for card setup and [`gateway/README.md`](gateway/README.md) for transcription and
Obsidian delivery.

Once Wi-Fi connects, open `http://recorder.local/` from another device on the
same network. If the network blocks mDNS, use the numeric address shown under
**Settings → Network → IP**. Audio-list and settings writes temporarily return
`503` while recording, playing, saving, or uploading so HTTP never contends
with real-time audio for the microSD bus.

To run host-side tests:

```sh
platformio test -d firmware -e native-tests
```

## Project layout

- `firmware/src/app`: recorder state flow, UI, settings, and screen saver.
- `firmware/src/hardware`: Cardputer ADV board, audio, power, and microSD.
- `firmware/src/media`: WAV parsing, writing, and filename helpers.
- `gateway/voice_gateway`: authenticated local backend and durable job queue.

## Controls

| Key | Action |
| --- | --- |
| `R` | Start recording |
| `P` in library | Cycle the note-processing profile |
| `S` in library | Cycle sorting: newest, oldest, processing status, name |
| `O` in library | Open Outbox |
| `I` in library | Open recording delivery details and transcript |
| `C` in library | Open Codex chat list |
| `L` in Codex | Return directly to the recorder library |
| `Enter` in chat list | Open selected conversation |
| `N` in chat list | Create a new local Codex task and type its first prompt |
| `V` in chat list | Create a new local Codex task and record its first prompt |
| `T` in Codex | Type a message to the selected chat |
| `R` in Codex | Record a voice message for the selected chat |
| `B` in Codex | Record once, save to Obsidian, and send to Codex |
| `E` in Codex conversation | Jump to the end of the conversation |
| `V` in Codex conversation | Speak the latest Codex response locally on the Mac |
| `A` in Codex conversation | Speak the recent conversation locally on the Mac |
| `X` while Codex works | Interrupt the current turn |
| `Enter` on an approval | Approve once |
| `S` on an approval | Approve for this Codex session |
| `D` / `X` on an approval | Decline / cancel the requesting turn |
| `H` in library | Open help |
| `H` in Outbox, details, Codex, or Settings | Open contextual help and return to the same screen |
| `Enter`, `Esc`, or `R` | Stop and save a recording |
| `P` or `Space` while recording | Pause or resume capture |
| `E` in recording details | Reprocess cached transcript with selected profile |
| `C` in recording details | Send cached transcript to a selected Codex task |
| `R` in recording details/Outbox | Retry processing or request upload |
| `A` in Outbox | Retry all failed gateway jobs for this Cardputer |
| Up/Down key positions (`;` / `.`) | Select a recording |
| `Enter` | Play the selected recording |
| `Enter` during playback | Pause or resume playback |
| `Esc` during playback | Stop playback |
| Left/Right during playback (`,` / `/`) | Seek backward or forward by the configured seek step |
| Up/Down during playback | Adjust volume |
| `[` / `]` during playback | Decrease or increase playback speed |
| Left in library (`,`) | Lock or unlock the selected recording |
| Right in library (`/`) | Rename the selected recording |
| `Delete` in library | Ask to delete the selected recording |
| `Enter` after `Delete` | Confirm deletion |
| Short `G0` press | Manually enter the configured screen saver mode |
| Long `G0` press | Open settings |
| Left/Right key positions (`,` / `/`) | Change a setting value |
| `Esc` in settings | Save settings or return from a submenu |
| `S` in Settings / Network | Scan visible Wi-Fi networks |
| Up/Down in Wi-Fi scan | Select a visible network |
| `Enter` in Wi-Fi scan | Connect, or enter the selected network password |
| `Enter` in rename | Save the new name |
| `Delete` in rename | Remove the last character |
| `Esc` in rename | Cancel rename |
| Left/Right in help | Change help page |
| `Enter` or `Esc` in help | Close help |

## SD-card configuration

All persistent configuration is portable with the card:

- `/RECORDER.CFG`: brightness, screen modes, low-battery save, seeking, wake
  behavior, library sorting, Codex/transcript font sizes, and wrapped Codex task
  names. The firmware creates it on first boot and updates it from the UI.
- `/AGENT.CFG`: Wi-Fi credentials, HTTPS gateway/discovery settings, device
  identity/token, and ordered voice-processing profiles.
- `/AGENT_WIFI.CFG`: extra Wi-Fi networks added from the scan screen. It is
  written only after a successful connection and may be edited or deleted on
  another computer.
- `/AGENT_CA.PEM`: trusted CA for the user's HTTPS gateway. Public firmware has
  no developer-specific trust anchor; legacy `/VOICE_CA.PEM` remains supported.
- `/VOICEAGENT.SENT`: automatically maintained upload-delivery ledger.
- `/*.ROUTE`: per-recording destination, processing profile, and Codex thread
  id. These files are created and maintained automatically.
- `/*.AGENT.JSON`: durable voice-job ID, stage, progress, attempts, error,
  Obsidian/Codex results, and transcript for the details screen.

Legacy `/VOICEAGENT.CFG` and `/VOICE_CA.PEM` names remain supported.

Before the first HTTPS request, Agent Console synchronizes its clock over NTP.
This is required for certificate validity checks; the Network/Codex status will
show `TLS CLOCK NOT SET` if the connected network cannot provide Internet/NTP.

Open **Settings → Services** to run an authenticated end-to-end check. The
screen reports Gateway, Whisper, Codex, transcript editor, and the number of
recordings still queued on SD. Press Enter to refresh.

Fallback networks use numbered keys: `wifi_ssid_2`/`wifi_password_2` through
`wifi_ssid_5`/`wifi_password_5`. The unnumbered pair remains profile 1.
Agent Console can hold up to five additional networks saved from the scan
screen. Quoted SSIDs and passwords are supported in both configuration files.

Set `gateway_base_url=auto` to discover `_cardputer-agent._tcp` on the current
LAN. Keep `gateway_fallback_url=https://…` for networks where multicast DNS is
blocked. Certificate validation remains mandatory, so the gateway certificate
must cover the discovered address.

`voice_profile` through `voice_profile_6` define the order cycled by `P`.
Built-in profiles are `default`, `meeting`, `idea`, and `task`; they write to
the configured Cardputer inbox and its `Meetings`, `Ideas`, or `Tasks`
subfolder.

Provider secrets such as an OpenAI, Claude, or Hermes gateway key stay on the
gateway computer and are never copied to the Cardputer. Templates are in
[`firmware/sdcard/`](firmware/sdcard/).

The Obsidian Vault is a gateway setting, not a firmware constant. Run
`python3 gateway/scripts/configure.py` to select a detected Vault or enter any
writable absolute path. The generated `gateway/.env` is private and ignored by
Git; `OBSIDIAN_NOTES_FOLDER` controls the destination inside that Vault.

## Recorder settings

Settings are saved to `/RECORDER.CFG` and restored after reboot. Brightness is
applied immediately. Screen saver options are grouped under `Screen Saver`:

| Setting | Values |
| --- | --- |
| Brightness | 10% through 100%, in 10% steps |
| Network | Profile, SSID, IP, last disconnect reason and last scan result; `S` scans and Enter retries |
| Compact Audio | 16 kHz standard, 8 kHz low-bandwidth PCM |
| Reading / Codex Chat | 1x, 2x |
| Reading / Transcripts | 1x, 2x |
| Reading / Chat Names | Full, 1 Line |
| Library Sort | Newest, Oldest, Status, A-Z |
| Low Battery Save | Off, 1%, 5%, 10% |
| Seek Step | 5 sec, 10 sec, 20 sec, 60 sec |
| Reset to Default | Restores saved settings after confirmation |
| Version | Current firmware version |
| Help | Opens the complete ten-page on-device control reference |
| Screen Saver / When Home | Off, Dimmed Standby, Black |
| Screen Saver / While Recording | Off, Dimmed Standby, Black |
| Screen Saver / While Playing | Off, Dimmed Standby, Black |
| Screen Saver / Triple-Press Wake | Off, On |
| Screen Saver / Visual Style | Cyber Grid, Data Rain |

`Low Battery Save` defaults to `10%`. When it is enabled and battery data is
valid, recording stops through the normal save path once the battery reaches
the selected threshold.

`Dimmed Standby` shows a low-brightness status screen. `Black` turns the
display off. Recording and playback continue in both modes; the first key
press wakes the screen and is not used as a stop, delete, or volume command.
When `Triple-Press Wake` is on, the same key must be pressed three times to
wake from `Dimmed Standby` or `Black`.

`Visual Style` chooses between the original neon city and perspective grid or
the animated green Data Rain. Both are generated procedurally and continue to
show recording/playback state without loading image assets from the SD card.

`Reset to Default` asks for confirmation before restoring brightness, screen
saver, triple-press wake, low battery save, and seek step to their defaults.

Playback speed starts at `1.0x` each time playback begins and can be changed
live from `0.75x` through `2.0x`. Speed changes adjust the playback sample
rate, so pitch changes with speed.

## File management

Press `S` in the library or use `Settings → Library Sort` to order recordings
by newest/oldest SD timestamp, processing status, or filename. Status
sorting puts failed and active jobs ahead of queued, delivered, and completed
transcripts. When an SD timestamp is unavailable, filename order is used as a
stable fallback. The selected mode is stored in `RECORDER.CFG` and restored
after reboot.

After transcription completes, the gateway generates a concise title. Notes
are stored as `YYYY-MM-DD HH-MM-SS - Title - hash.md`; the Cardputer renames the
matching WAV to `YYYY-MM-DD HH-MM-SS - Title.WAV` only after the durable job is
complete. Route and metadata sidecars move with the recording; an existing
filename is never overwritten.

Locked recordings show `*` before the filename and cannot be deleted until
unlocked. Lock state is stored on the card in `RECORDER.LCK`; if the file is
missing or damaged, recordings remain usable.

Rename keeps the `.WAV` suffix automatically. Names are converted to uppercase
and accept letters, numbers, spaces, `_`, and `-`. Existing files are never
overwritten.

## Audio format

New recordings use 16 kHz mono 16-bit PCM by default. Compact Audio uses 8 kHz
mono 16-bit PCM, cutting size and upload time in half while remaining a normal
WAV; switch back to 16 kHz when recognition quality matters more than
bandwidth. Playback accepts mono 16-bit PCM WAV files and walks RIFF chunks
instead of requiring audio to begin at byte 44.

See [audio and storage notes](docs/audio-and-storage.md),
[troubleshooting](docs/troubleshooting.md), and the
[hardware test checklist](docs/hardware-test-checklist.md).

## License

[MIT](LICENSE)
