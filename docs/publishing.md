# Publishing checklist

This repository is structured so source and distributable firmware can be
published without personal device data.

## Before the first push

1. Review `git status` and confirm `backups/`, `third_party/`, `dist/`,
   `gateway/.env`, `gateway/certs/`, and `gateway/data/` are absent.
2. Search tracked files for personal paths, tokens, Wi-Fi passwords, recordings,
   transcripts, private keys, and email addresses.
3. Run both firmware and gateway test suites.
4. Verify the M5Apps image checksum and test installation on a Cardputer ADV.
5. Create a new GitHub repository, replace the inherited upstream remote, and
   push only after reviewing the complete staged diff.

## Release model

The current verified binary is kept in `release/firmware` for convenience.
Future tagged builds are produced by `.github/workflows/release.yml` and
attached to GitHub Releases with `SHA256SUMS.txt`. Do not add partition dumps,
full-flash images, recordings, or third-party applications to releases.

The project began from `Moskic/cardputer-adv-recorder`; retain the upstream
license and attribution when publishing the fork.

## Project website

The product site lives in `site/` and has its own build instructions. It embeds
the current firmware and checksum under `site/public/downloads`; update those
copies whenever a new canonical file is added to `release/firmware`. The Sites
project identifier remains local in the ignored `site/.openai/hosting.json`;
deployment credentials and generated output are also excluded. GitHub Pages is
built from the same page component by `.github/workflows/pages.yml`, so both
hosted versions stay in sync.
