# Cardputer microSD setup

1. Format an SDHC card (32 GB or smaller) as FAT32 with an MBR partition map.
2. Copy `AGENT.CFG.example` to the card root as `AGENT.CFG` and edit it.
   Recorder UI and behavior are stored separately in `RECORDER.CFG`; it is
   created automatically, or can be seeded from `RECORDER.CFG.example`.
   Library sorting is saved there as `library_sort`; press `S` in the library
   to cycle newest, oldest, status, and filename order.
   Screen saver appearance is stored as `screen_style`: `0` selects Cyber
   Grid and `1` selects Data Rain. It is easier to change from the on-device
   `Settings → Screen Saver → Visual Style` menu.
   `codex_names_full=true` shows full wrapped Codex task names;
   switch it under `Settings → Reading → Chat Names`.
   `web_enabled=true` enables the same-LAN administration page at
   `http://recorder.local/`; change `web_hostname` to use another `.local`
   name. The panel lists/downloads WAV files, shows device/network/storage
   status, and edits common recorder settings.
   Add `wifi_ssid_2`/`wifi_password_2` through profile 5 for fallback
   networks. Agent Console tries them in numerical order, shows the active
   profile and connected SSID under `Settings → Network`, and starts over
   after a pause when every profile fails.
   Press `S` on that page to scan visible networks. The scan list shows signal
   strength, channel, and security. Select a network with Up/Down and Enter,
   then type its password and press Enter. A successfully connected extra
   network is saved automatically as `/AGENT_WIFI.CFG` and joined to the retry
   list on later boots. Up to five such extra networks are retained.
   Voice processing profiles are stored as `voice_profile` through
   `voice_profile_6`; press `P` in the library to cycle them before recording.
   `gateway_base_url=auto` enables Bonjour discovery, while
   `gateway_fallback_url` keeps a known HTTPS endpoint as fallback.
3. For an HTTPS gateway, also copy the gateway CA certificate to the root as
   `AGENT_CA.PEM`. The firmware refuses unverified HTTPS. Legacy
   `VOICEAGENT.CFG` and `VOICE_CA.PEM` names remain readable.
4. Copy the app-only PlatformIO output `firmware.bin` to the card. Install that
   binary from M5Apps; do not use a merged/full image because that would replace
   M5Apps itself.
5. For BrokenSignal v2, place music under `/Music`. Album folders may be nested
   to any depth; the player accepts `.mp3` and `.m4a` (AAC-LC). Its volume,
   theme, shuffle, repeat, Wi-Fi, and radio settings are also kept below
   `/Music`.

`VOICEAGENT.SENT` is created automatically. It is an append-only delivery ledger;
the gateway additionally deduplicates recordings by SHA-256. Each new WAV also
gets a small `.ROUTE` sidecar containing `note`, `codex`, or `both`, the profile,
and the exact Codex thread id selected at recording time. This keeps offline
delivery stable when the active chat changes later. After gateway acceptance,
Agent Console writes a matching `.AGENT.JSON` sidecar containing the durable
job ID, stage/progress, attempts, errors, Obsidian/Codex results, and transcript.
It updates the sent ledger only after the asynchronous job reaches `completed`.
At that point it also applies the gateway's dated, transcript-derived WAV name
and moves the matching route and metadata sidecars. Existing names are never
overwritten.

The Network page also shows the most recent ESP32 disconnect reason. Common
results include `NO_AP_FOUND (201)` when the SSID is not visible and
`AUTH_FAIL (202)` when authentication fails. `RADIO: NOT SEEN` after a scan
usually indicates a disabled hotspot, unsupported band, or insufficient signal.
The numeric IP on this page is a fallback when a router or client blocks mDNS.
