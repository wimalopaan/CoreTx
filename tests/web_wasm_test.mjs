// SPDX-License-Identifier: GPL-2.0-or-later
import assert from "node:assert/strict";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

for (const directory of process.argv.slice(2)) {
  const profile = directory.includes("i6s") ? "i6s" : "i6x";
  const { default: create } = await import(pathToFileURL(resolve(directory, `coretx-${profile}.js`)));
  const module = await create();
  const address = module._sim_eeprom();
  module.HEAPU8.fill(255, address, address + 16384);
  module._sim_init();
  assert.equal(module._sim_status() & 4, 0, "Startup blocks outputs");
  for (let i = 0; i < 50; i++) module._sim_step(100);
  assert.equal(module._sim_status() & 7, 7, module.UTF8ToString(module._sim_error()));
  assert.equal(module._sim_busy(), 0);
  module._sim_set_analog(0, 0);
  module._sim_step(100);
  assert.equal(module.HEAP16[module._sim_channels() / 2], 1024);
  const snapshot = module.HEAPU8.slice(address, address + 16384);
  module._sim_init();
  for (let i = 0; i < 50; i++) module._sim_step(100);
  assert.deepEqual(module.HEAPU8.slice(address, address + 16384), snapshot);
  assert.ok(module.HEAPU8.slice(module._sim_lcd(), module._sim_lcd() + 8192).some(Boolean));

  // Use native records to exercise the actual WASM dashboard, not a JavaScript mock.
  const recordSize = 768;
  const record = address + 2304;
  const page = record + 34 + 2 + module.HEAPU8[record + 34] * 7 +
    module.HEAPU8[record + 35] * 6 + 16 * 5 + 8 + 8 * 7;
  const seal = () => {
    let crc = 0xffffffff;
    for (const byte of module.HEAPU8.subarray(record, record + recordSize - 4)) {
      crc ^= byte;
      for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    new DataView(module.HEAPU8.buffer).setUint32(record + recordSize - 4, (crc ^ 0xffffffff) >>> 0, true);
  };
  const step = (count = 1) => { for (let i = 0; i < count; i++) module._sim_step(100); };
  const touch = (x, y) => {
    module._sim_touch(x, y, 1);
    step();
    module._sim_touch(x, y, 0);
    step();
  };
  const pixel = (x, y) => module.HEAPU8[module._sim_lcd() + y * 128 + x];
  for (const [layout, topGap, middle] of [[0, 0, 1], [1, 1, 1], [2, 1, 0]]) {
    module.HEAPU8.set(snapshot, address);
    // Format 12: address 0, layout in bits 9..10.
    module.HEAPU8[page] = 0;
    module.HEAPU8[page + 1] = layout * 2;
    seal();
    module._sim_init();
    step(50);
    assert.equal(module._sim_status() & 7, 7, "Layout fixture loads");
    if (profile === "i6x") {
      module._sim_key(1, 1);
      step();
      module._sim_key(1, 0);
      step();
    } else {
      for (let i = 0; i < 2; i++) {
        module._sim_touch(80, 30, 1);
        step();
        module._sim_touch(40, 30, 1);
        step();
        module._sim_touch(40, 30, 0);
        step();
      }
    }
    step(2);
    assert.equal(pixel(31, 14), topGap, `${profile}: layout ${layout} column geometry`);
    assert.equal(pixel(5, 32), middle, `${profile}: layout ${layout} row geometry`);
    if (profile === "i6s" && layout === 2) {
      touch(80, 40);
      step();
      assert.equal(pixel(67, 42), 1, "i6S: enlarged tile responds to touch");
    }
  }
  console.log(`${profile}: WASM application, controls, LCD and native EEPROM passed`);
}
