# Recommended Cardputer ADV work apps

Reviewed against the M5Burner catalog on 2026-08-09. Prefer binaries explicitly marked for **Cardputer ADV**;
the original Cardputer uses different keyboard and audio hardware.

## Install first

1. **Cardputer ADV Voice Agent** — offline-first recording, HTTPS delivery,
   transcription, and Obsidian capture. Keep it in flash because it is the main
   capture tool.
2. **Finder** — M5Apps' built-in two-panel SD/USB file manager. Useful for
   checking recordings and configuration without removing the card.
3. **M5Apps Settings / Wi-Fi Scanner** — keep network setup and basic radio
   diagnosis available even if another app's configuration is broken.
4. **Flood** — M5Apps' built-in ESP-NOW mesh chat. It is useful for short local
   messages when normal Wi-Fi infrastructure is unavailable.
5. **BrokenSignal v2** — the preferred current music player for Cardputer ADV.
   It has arbitrary-depth folders below `/Music`, MP3 and M4A/AAC-LC playback,
   pagination for 200+ tracks, recent tracks, repeat/shuffle, persistent volume
   and theme settings, and optional web radio. Unlike the more-starred generic
   Cardputer player, its upstream target explicitly says Cardputer ADV:
   [MarcoRR/BrokenSignal](https://github.com/MarcoRR/BrokenSignal).

## Useful when needed

- **SSH Client - Cardputer & ADV 1.1** — the current M5Burner catalog now has an
  ADV-specific client with ANSI/VT100, multiple SD-card profiles, and optional
  WireGuard support. Install it before the older generic SSH entries. Do not
  add production private keys until host-key verification and key storage have
  been checked on-device.
- **ESP32 Bit Pirate** — excellent for authorized hardware debugging: UART,
  I2C, SPI, 1-Wire, JTAG, USB serial, Wi-Fi, and Bluetooth tools. The current
  project explicitly supports Cardputer ADV:
  [geo-tp/ESP32-Bit-Pirate](https://github.com/geo-tp/ESP32-Bit-Pirate).
- **WiFi & BLE Radar ADV 3.1** — useful read-only radio and 2.4 GHz channel
  diagnostics. Prefer this to broad offensive-security bundles.
- **Ultimate Remote - Cardputer & ADV 1.2** — useful if IR control is part of
  the job; it supports Flipper-IRDB files from SD. Use only profiles for
  equipment you control.
- **M5Gemini 3.0** — now explicitly supports ADV and requires one Google key.
  It is suitable for testing the interface, but a custom Agent Console through
  the HTTPS gateway is still safer for Codex, Claude, or Hermes.
- **Claude desktop buddy 1.0.0** — an optional BLE companion for Claude Code
  permission approvals. It is not a Claude chat client and requires Claude's
  developer-mode Hardware Buddy support on the Mac.

## Skip for this workflow

- **WiFi Remote Display ADV** is Windows-oriented and injects a PowerShell/Python
  payload through USB HID, so it is not a good fit for a Mac-based daily setup.
- Broad offensive-security bundles such as Bruce or Poseidon are not needed for
  voice capture or server administration. If installed for a lab, use them only
  against devices and networks you own or are explicitly authorized to test.
- Avoid filling the 8 MB flash with games and demos. Keep infrequent binaries on
  SD and use M5Apps FDISK to remove unused app partitions.
- Do not reinstall `Cardputer-MP3-ADV-AudioFix-v3`: it advanced playback but
  produced no audio on the tested ADV hardware.

## Best next custom app

Build a single **Agent Console** with profiles in `/AGENTS.CFG`:

- `name=Codex`, `name=Claude`, and `name=Hermes` profiles;
- one HTTPS gateway URL and device token, with provider credentials only on the
  gateway;
- streaming text chat, saved prompts, and an option to attach the latest voice
  transcript;
- no direct OpenAI, Anthropic, or Hermes secrets stored on the Cardputer.

As of the review date, the M5Burner Cardputer catalog has no standalone Codex
client and no Hermes connector. Claude desktop buddy is an approval companion,
not a general Claude client.
