import assert from "node:assert/strict";
import { createHash } from "node:crypto";

export function assertSha256(binary, expected) {
  const actual = createHash("sha256").update(binary).digest("hex");
  assert.equal(actual, expected, "SHA-256 mismatch: binary differs from checksum manifest");
}
