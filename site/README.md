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


`npm ci && npm run lint && npm test` checks a clean checkout without local Sites
metadata. Tests build the static Pages target and verify source contracts and
the actual SHA-256 of the bundled firmware. `npm run build` remains the separate
Workers/Sites build and requires the local ignored hosting configuration.
Neither test command publishes a site or firmware release.

### Maintenance dependency limits (2026-10-03)

The site uses React 19.3.0, Vite 8.3.2 and vinext 1.0.1 with a refreshed lockfile.
ESLint stays on the latest 9.x patch because the current React and accessibility
plugins do not declare ESLint 10 support. ESLint 9 is now upstream-deprecated;
a supported-major migration remains follow-up work. TypeScript stays at 6.0.3 because
`typescript-eslint` requires a version below 6.1. Node types track the supported
22.x minimum. These are compatibility constraints, not a claim that every tool
uses the newest major version.

A scoped `satori` override selects fflate 0.7.5 to address malformed-ZIP parsing.
The production-only npm audit is clean. The full development audit still reports
11 findings (7 high, 4 moderate) through the currently published braces 3.0.3 and
Drizzle's legacy esbuild tooling. Automatic suggested fixes would downgrade or
break direct tools; they were not forced. Run `npm audit` before using development
servers with untrusted content. Static Pages tests do not validate the separate
Workers/Sites deployment or optional database tooling.

The updated Workers build also compiles locally with a temporary synthetic
`{ "d1": null, "r2": null }` hosting configuration. That checks the vinext build
path only; no actual Sites bindings, database, image service or deployment were
used, and the temporary configuration is not committed.
