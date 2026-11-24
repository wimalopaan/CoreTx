// SPDX-License-Identifier: GPL-2.0-or-later
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { resolve, extname, sep } from "node:path";

const { chromium } = await import(process.env.CORETX_PLAYWRIGHT || "playwright");
const root = resolve(process.argv[2] || "_site");
const server = createServer(async (request, response) => {
  const file = resolve(root, "." + new URL(request.url, "http://localhost").pathname.replace(/\/$/, "/index.html"));
  if (!file.startsWith(root + sep)) {
    response.writeHead(403).end();
    return;
  }
  try {
    const bytes = await readFile(file);
    response.setHeader("Content-Type", {
      ".js": "text/javascript", ".html": "text/html", ".css": "text/css",
      ".wasm": "application/wasm",
    }[extname(file)] || "application/octet-stream");
    response.end(bytes);
  } catch (error) {
    response.writeHead(error.code === "ENOENT" ? 404 : 500).end(String(error));
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const url = `http://127.0.0.1:${server.address().port}/`;
let browser;
try {
  browser = await chromium.launch({
    executablePath: process.env.CORETX_CHROMIUM || undefined,
    args: ["--no-sandbox"],
  });
  const context = await browser.newContext({ acceptDownloads: true });
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", error => errors.push(error.message));
  page.on("dialog", dialog => dialog.accept());
  const key = "coretx.simulator.eeprom.v1.";
  await page.goto(url);
  await page.waitForFunction(() => document.querySelector("#output-state").textContent.startsWith("Gueltige"));
  assert.equal(await page.locator(".channel").count(), 16);
  await page.locator("#axis-0").fill("0");
  await page.waitForFunction(() => document.querySelector(".channel-value").textContent === "+100.0%");
  await page.locator("#axis-0").fill("2048");
  await page.getByRole("checkbox", { name: "RH automatisch zentrieren", exact: true }).uncheck();

  await page.locator(".stick-pad").first().scrollIntoViewIfNeeded();
  const stick = await page.locator(".stick-pad").first().boundingBox();
  await page.mouse.move(stick.x + stick.width / 2, stick.y + stick.height / 2);
  await page.mouse.down();
  await page.mouse.move(stick.x + stick.width - 3, stick.y + 3);
  await page.waitForFunction(() => Number(document.querySelector("#axis-3").value) > 4000);
  await page.mouse.up();
  await page.waitForFunction(() => document.querySelector("#axis-3").value === "2048");

  await page.waitForFunction(storageKey => localStorage.getItem(storageKey) !== null, key + "i6x");
  const trim = page.getByRole("button", { name: "Trimmung RH erhoehen", exact: true });
  await trim.hover();
  await page.mouse.down();
  await page.waitForTimeout(100);
  await page.mouse.up();
  await page.waitForFunction(() => document.querySelector(".channel-value").textContent !== "0.0%");
  await page.waitForFunction(() => document.querySelector("#storage-status").textContent === "Browser-Stand gesichert");
  const edited = await page.locator(".channel-value").first().textContent();
  const saved = await page.evaluate(storageKey => localStorage.getItem(storageKey), key + "i6x");
  await page.reload();
  await page.waitForFunction(() => document.querySelector("#output-state").textContent.startsWith("Gueltige"));
  assert.equal(await page.locator(".channel-value").first().textContent(), edited);
  assert.equal(await page.getByRole("checkbox", { name: "RH automatisch zentrieren", exact: true }).isChecked(), false);
  await page.locator("#import-file").setInputFiles({
    name: "bad.bin", mimeType: "application/octet-stream", buffer: Buffer.alloc(3),
  });
  await page.waitForFunction(() => document.querySelector("#notice").textContent.includes("Import abgelehnt"));
  assert.equal(await page.evaluate(storageKey => localStorage.getItem(storageKey), key + "i6x"), saved);

  const downloadEvent = page.waitForEvent("download");
  await page.locator("#download").click();
  const download = await downloadEvent;
  const bytes = await readFile(await download.path());
  assert.equal(bytes.length, 16384);
  assert.equal(bytes.toString("base64"), saved);
  await page.locator("#import-file").setInputFiles({
    name: "saved.bin", mimeType: "application/octet-stream", buffer: bytes,
  });
  await page.waitForFunction(() => !document.querySelector("#commit-import").hidden &&
    !document.querySelector("#commit-import").disabled);
  assert.equal(await page.evaluate(storageKey => localStorage.getItem(storageKey), key + "i6x"), saved);
  await page.locator("#commit-import").click();
  await page.waitForFunction(() => document.querySelector("#commit-import").hidden);

  const future = Buffer.from(bytes);
  for (const address of [2304, 2304 + 768]) {
    if (future.toString("ascii", address, address + 4) !== "MDL1") continue;
    future[address + 4] = 255;
    let crc = 0xffffffff;
    for (const byte of future.subarray(address, address + 764)) {
      crc ^= byte;
      for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    future.writeUInt32LE((crc ^ 0xffffffff) >>> 0, address + 764);
  }
  await page.locator("#import-file").setInputFiles({
    name: "future.bin", mimeType: "application/octet-stream", buffer: future,
  });
  await page.waitForFunction(() => document.querySelector("#run-status").textContent.startsWith("Wiederherstellung"));
  assert.ok(await page.locator("#commit-import").isDisabled());
  assert.equal(await page.evaluate(storageKey => localStorage.getItem(storageKey), key + "i6x"), saved);
  await page.locator("#restore").click();
  await page.waitForFunction(() => document.querySelector("#output-state").textContent.startsWith("Gueltige"));

  const beforeMenu = await page.locator("#lcd").evaluate(canvas => canvas.toDataURL());
  await page.locator('[data-key="7"]').click();
  await page.waitForFunction(before => document.querySelector("#lcd").toDataURL() !== before, beforeMenu);
  await page.locator('[data-key="8"]').click();
  await page.locator("#profile").selectOption("i6s");
  await page.waitForFunction(() => document.querySelector("#profile-badge").textContent === "FS-i6S" &&
    document.querySelector("#output-state").textContent.startsWith("Gueltige"));
  assert.equal(await page.locator(".rear-controls button").count(), 2);
  assert.equal(await page.getByRole("checkbox", { name: "RH automatisch zentrieren", exact: true }).isChecked(), true);
  await page.locator("#lcd").scrollIntoViewIfNeeded();
  const lcd = await page.locator("#lcd").boundingBox();
  const beforeTouch = await page.locator("#lcd").evaluate(canvas => canvas.toDataURL());
  await page.mouse.move(lcd.x + lcd.width / 2, lcd.y + lcd.height * .6);
  await page.mouse.down();
  await page.waitForTimeout(80);
  await page.mouse.move(lcd.x + lcd.width * .9, lcd.y + lcd.height * .6, { steps: 4 });
  await page.mouse.up();
  await page.waitForFunction(before => document.querySelector("#lcd").toDataURL() !== before, beforeTouch);
  await page.locator('[data-key="8"]').click();
  await page.locator("#profile").selectOption("i6x");
  await page.waitForFunction(() => document.querySelector("#profile-badge").textContent === "FS-i6X" &&
    document.querySelector("#output-state").textContent.startsWith("Gueltige"));
  assert.equal(await page.locator(".channel-value").first().textContent(), edited);
  assert.equal(await page.getByRole("checkbox", { name: "RH automatisch zentrieren", exact: true }).isChecked(), false);

  await page.setViewportSize({ width: 390, height: 844 });
  assert.ok(await page.locator("#lcd").isVisible());
  await page.waitForFunction(() => document.documentElement.scrollWidth <= innerWidth);
  const other = await page.context().newPage();
  await other.goto(url);
  await other.waitForFunction(() => document.querySelector("#output-state").textContent.startsWith("Gueltige"));
  const concurrent = Buffer.from(bytes);
  concurrent[16383] ^= 1;
  const newer = concurrent.toString("base64");
  await other.evaluate(({ storageKey, value }) => localStorage.setItem(storageKey, value),
    { storageKey: key + "i6x", value: newer });
  await page.waitForFunction(() => document.querySelector("#storage-warning").textContent.includes("anderen Fenster"));
  assert.equal(await page.evaluate(storageKey => localStorage.getItem(storageKey), key + "i6x"), newer);
  await other.close();
  assert.deepEqual(errors, []);
  await page.close();
  console.log("Browser controls, LCD/touch, channels, profiles and file persistence passed");
} finally {
  if (browser) await browser.close();
  await new Promise(resolve => server.close(resolve));
}
