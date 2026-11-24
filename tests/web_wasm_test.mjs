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
  console.log(`${profile}: WASM application, controls, LCD and native EEPROM passed`);
}
