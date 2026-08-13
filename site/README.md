# Cardputer Agent Console site

Product site for Cardputer ADV Agent Console. It explains the local-first
workflow, displays real device screens, and ships the verified app-only 2.8.1
firmware download.

```sh
npm install
npm run dev
npm run build
npm run build:pages
```

The site is built with vinext for Cloudflare Workers and deployed with Sites.
Local Sites project metadata lives in the ignored `.openai/hosting.json`; no
runtime secrets are required. The separate `build:pages` command creates the
static `pages-dist/` artifact used by GitHub Pages. Firmware and checksums in
`public/downloads/` must match the canonical artifacts in the repository's
`release/firmware/` directory.
