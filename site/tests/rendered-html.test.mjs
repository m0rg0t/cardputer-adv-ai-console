import assert from "node:assert/strict";
import { readFile, stat } from "node:fs/promises";
import { test } from "node:test";
import { assertSha256 } from "./checksum.mjs";

const root = new URL("../", import.meta.url);

test("product page contains primary sections and current release", async () => {
  const page = await readFile(new URL("app/page.tsx", root), "utf8");
  assert.match(page, /Cardputer-ADV-Agent-Console-2\.8\.1-M5Apps\.bin/);
  assert.match(page, /id="workflow"/);
  assert.match(page, /id="features"/);
  assert.match(page, /id="install"/);
  assert.match(page, /Local Gateway/);
  assert.match(page, /cardputer-adv-front-clean\.webp/);
  assert.match(page, /docs\.m5stack\.com\/en\/core\/Cardputer-Adv/);
  assert.doesNotMatch(page, /SkeletonPreview|codex-preview/);
});

test("metadata uses a real official Cardputer ADV photograph", async () => {
  const layout = await readFile(new URL("app/layout.tsx", root), "utf8");
  assert.match(layout, /Cardputer ADV Agent Console/);
  assert.match(layout, /cardputer-adv-angle\.webp/);
  assert.ok((await stat(new URL("public/images/official/cardputer-adv-angle.webp", root))).size > 50_000);
});

test("status colors stay scoped and feature cards keep readable backgrounds", async () => {
  const css = await readFile(new URL("app/globals.css", root), "utf8");
  assert.match(css, /\.terminal-dot\.amber/);
  assert.match(css, /\.feature-card\.amber[^}]+linear-gradient/);
  assert.doesNotMatch(css, /(?<![\w.-])\.amber\s*\{\s*background:/);
  assert.doesNotMatch(css, /(?<![\w.-])\.green\s*\{\s*background:/);
});

test("interface screenshots preserve their full 16:9 frame", async () => {
  const css = await readFile(new URL("app/globals.css", root), "utf8");
  assert.match(css, /\.screen-grid img\s*\{[^}]*aspect-ratio:\s*16\/9[^}]*object-fit:\s*contain/s);
  assert.doesNotMatch(css, /\.screen-grid img\s*\{[^}]*object-fit:\s*cover/s);
});

test("download matches the published checksum manifest", async () => {
  const manifest = await readFile(new URL("public/downloads/SHA256SUMS.txt", root), "utf8");
  assert.match(manifest, /^[a-f0-9]{64}\s+Cardputer-ADV-Agent-Console-2\.8\.1-M5Apps\.bin\s*$/);
  const binary = await readFile(new URL("public/downloads/Cardputer-ADV-Agent-Console-2.8.1-M5Apps.bin", root));
  assert.ok(binary.length > 1_000_000);
  assertSha256(binary, manifest.trim().split(/\s+/)[0]);
});


test("checksum validation rejects a changed binary", () => {
  // SHA-256 of the synthetic ASCII fixture "abc".
  const expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  assertSha256(Buffer.from("abc"), expected);
  assert.throws(() => assertSha256(Buffer.from("abd"), expected), /SHA-256 mismatch/);
});
