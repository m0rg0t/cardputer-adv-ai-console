# Security

The Cardputer connects to a user-operated gateway. Device tokens, Wi-Fi
passwords, API keys, TLS private keys, recordings, transcripts, Codex task
history, and absolute personal paths must never be committed.

- Keep gateway secrets in `gateway/.env` (mode `0600`) or the macOS Keychain.
- Keep Cardputer credentials only in the ignored real SD-card configuration.
- Use HTTPS and a certificate trusted by the firmware.
- Bind the gateway only to networks you trust; it is not designed for direct
  public-Internet exposure.
- Review generated Obsidian notes before sharing them: they may contain private
  speech and local file names.

If you discover a vulnerability, report it privately to the repository owner
instead of opening a public issue containing exploit details or credentials.
