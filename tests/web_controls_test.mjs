// SPDX-License-Identifier: GPL-2.0-or-later
import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

// The deployed JS is a browser ES module; no repository-wide Node package mode is needed.
const source = await readFile(new URL("../platform/web/simulator.js", import.meta.url), "utf8");
const { decodeEEPROM, encodeEEPROM, lcdPoint, channelPresentation, stickCoordinates } =
  await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);

test("EEPROM serialization round-trips all bytes without accepting malformed storage", () => {
  const bytes = Uint8Array.from({ length: 16384 }, (_, i) => i % 256);
  const encoded = encodeEEPROM(bytes);
  assert.deepEqual(decodeEEPROM(encoded), bytes);
  for (const invalid of [null, "", encoded.slice(4), ` ${encoded}`, encoded.replace(/=+$/, "AA"),
    `${encoded.slice(0, -3)}B==`]) {
    assert.throws(() => decodeEEPROM(invalid));
  }
  assert.throws(() => encodeEEPROM(new Uint8Array(16383)));
});

test("LCD touch is mapped from the actual canvas bounds and clamped at edges", () => {
  const rect = { left: 30, top: 20, width: 384, height: 192 };
  assert.deepEqual(lcdPoint(30, 20, rect), [0, 0]);
  assert.deepEqual(lcdPoint(222, 116, rect), [64, 32]);
  assert.deepEqual(lcdPoint(414, 212, rect), [127, 63]);
  assert.deepEqual(lcdPoint(-10, 999, rect), [0, 63]);
});

test("channel numbers retain overtravel while bar extents stay within 150 percent", () => {
  assert.deepEqual(channelPresentation(1024), { percent: 100, left: 50, width: 100 / 3 });
  assert.deepEqual(channelPresentation(-2048), { percent: -200, left: 0, width: 50 });
  assert.deepEqual(channelPresentation(2048), { percent: 200, left: 50, width: 50 });
  assert.deepEqual(channelPresentation(0), { percent: 0, left: 50, width: 0 });
});

test("physical stick ADC directions account for reversed right-hand sensors", () => {
  const rect = { left: 10, top: 20, width: 200, height: 200 };
  assert.deepEqual(stickCoordinates(10, 20, rect, false), [0, 4095]);
  assert.deepEqual(stickCoordinates(210, 220, rect, false), [4095, 0]);
  assert.deepEqual(stickCoordinates(10, 20, rect, true), [4095, 0]);
  assert.deepEqual(stickCoordinates(210, 220, rect, true), [0, 4095]);
  assert.deepEqual(stickCoordinates(110, 120, rect, false), [2048, 2048]);
  assert.deepEqual(stickCoordinates(-500, 999, rect, true), [4095, 4095]);
});
