// SPDX-License-Identifier: GPL-2.0-or-later
export const EEPROM_SIZE = 16384;
export const clamp = (value, low, high) => Math.min(high, Math.max(low, value));

export function decodeEEPROM(encoded) {
  // Reject partial decodes, whitespace and noncanonical padding, not just wrong lengths.
  if (typeof encoded !== "string" || encoded.length !== 21848 ||
      !/^[A-Za-z0-9+/]{21846}==$/.test(encoded)) {
    throw new Error("Ungueltige EEPROM-Daten im Browser-Speicher.");
  }
  const binary = atob(encoded);
  if (binary.length !== EEPROM_SIZE || btoa(binary) !== encoded) {
    throw new Error("Ungueltige EEPROM-Groesse oder Base64-Kodierung.");
  }
  return Uint8Array.from(binary, char => char.charCodeAt(0));
}

export function encodeEEPROM(bytes) {
  if (bytes.length !== EEPROM_SIZE) throw new Error("EEPROM muss genau 16384 Bytes enthalten.");
  return btoa(String.fromCharCode(...bytes));
}

export function lcdPoint(clientX, clientY, rect) {
  return [
    clamp(Math.floor((clientX - rect.left) * 128 / rect.width), 0, 127),
    clamp(Math.floor((clientY - rect.top) * 64 / rect.height), 0, 63),
  ];
}

export function channelPresentation(raw) {
  const percent = raw * 100 / 1024;
  const extent = Math.min(Math.abs(percent), 150) / 3;
  return { percent, left: percent < 0 ? 50 - extent : 50, width: extent };
}

export function stickCoordinates(clientX, clientY, rect, rightStick) {
  const x = clamp((clientX - rect.left) / rect.width, 0, 1);
  const y = clamp((clientY - rect.top) / rect.height, 0, 1);
  return [
    Math.round((rightStick ? 1 - x : x) * 4095),
    Math.round((rightStick ? y : 1 - y) * 4095),
  ];
}

const AXES = [
  ["RH", "Rechter Knueppel horizontal"],
  ["RV", "Rechter Knueppel vertikal"],
  ["LV", "Linker Knueppel vertikal"],
  ["LH", "Linker Knueppel horizontal"],
  ["VRA", "i6X: Drehpoti, i6S: Seitenslider A"],
  ["VRB", "i6X: Drehpoti, i6S: Seitenslider B"],
  ...["VRC", "VRD", "VRE", "VRF", "VRG"].map(name =>
    [name, "Zusaetzlicher Eingang, im Sendermenue aktivierbar"]),
];
// Sticks and VRA/VRB are operated on the transmitter; the panel lists the additional inputs.
const STICK_AXES = 4;
const PANEL_AXES = 6;
const DIGITAL_INPUTS = 5;
const DIGITAL_NAMES = { i6x: ["SE", "SF"], i6s: ["SG", "SH", "SN", "SO", "SP"] };
const STATUS = { loaded: 1, editable: 2, valid: 4, busy: 8, recovery: 16, off: 32, fatal: 64 };
const STORAGE_PREFIX = "coretx.simulator.eeprom.v1.";
const SETTINGS_PREFIX = "coretx.simulator.controls.v1.";

if (typeof document !== "undefined") startBrowser();

function startBrowser() {
  const $ = id => document.getElementById(id);
  const elements = Object.fromEntries([
    "profile", "expert-mode", "analog-panel", "storage-panel",
    "native-controls", "analog-controls", "switches", "sticks", "stick-centering",
    "trims-left", "trims-right", "axes", "lcd", "lcd-frame", "lcd-controls", "lcd-hint",
    "navigation", "control-hint", "pots",
    "channels", "output-state", "run-status", "storage-status", "rf-status", "notice",
    "storage-warning", "fatal", "download", "import-file", "commit-import", "restore",
    "reset", "save-detail",
  ].map(id => [id, $(id)]));
  const context = elements.lcd.getContext("2d", { alpha: false });
  const image = context?.createImageData(128, 64);
  const analogs = new Uint16Array(AXES.length).fill(2048);
  const switches = new Uint8Array(16);
  const springs = [true, true, true, true];
  const springInputs = [];
  const axisViews = [];
  const stickViews = [];
  const potViews = [];
  let digitalSwitches = null;
  let digitalModes = -1;
  let digitalPushes = [];
  let edges = [];
  const channelViews = [];
  const holds = new Map();
  const keyboardHolds = new Map();
  const navigationKeys = new Map();
  const pointerReleases = new Set();
  const dynamicReleases = new Set();
  let module = null;
  let initialized = false;
  let profile = "i6x";
  let analogCount = AXES.length;
  let changing = false;
  let staged = false;
  let protectedStorage = false;
  let persistentWarning = "";
  let flags = 0;
  let savedRevision = 0;
  let observedRevision = 0;
  let changedAt = 0;
  let saveRetryAt = 0;
  let lastFrame = null;
  let frame = null;
  let touch = null;
  let failed = false;
  let saveDetail = "";
  let savedEncoded = null;

  function notice(text, error = false) {
    elements.notice.textContent = text;
    elements.notice.classList.toggle("warning", error);
    elements.notice.hidden = !text;
  }

  function storageWarning(text) {
    persistentWarning = text;
    elements["storage-warning"].textContent = text;
    elements["storage-warning"].hidden = !text;
  }

  function detail(text) {
    if (saveDetail !== text) {
      saveDetail = text;
      elements["save-detail"].textContent = text;
    }
  }

  function fatal(error) {
    failed = true;
    stopFrames();
    releaseAll();
    elements.fatal.textContent = `Simulator angehalten: ${error.message || error}. ` +
      "Der gespeicherte Browser-Stand bleibt erhalten. Profil neu laden oder EEPROM sichern.";
    elements.fatal.hidden = false;
    elements["run-status"].textContent = "Firmware-Fehler";
    flags = STATUS.fatal;
    updateEnabled();
    renderChannels();
  }

  function nativeCall(callback) {
    if (!module || !initialized || failed) return;
    try { callback(); } catch (error) { fatal(error); }
  }

  function updateEnabled() {
    const active = initialized && !changing && !failed;
    elements["native-controls"].disabled = !active;
    elements["analog-controls"].disabled = !active;
    elements.profile.disabled = changing;
    elements["import-file"].disabled = !module || changing;
    elements.download.disabled = !initialized || changing;
    elements.reset.disabled = !module || changing;
    elements.restore.disabled = !module || changing;
    elements["commit-import"].hidden = !staged;
    elements["commit-import"].disabled = !active ||
      !(flags & STATUS.loaded) || Boolean(flags & (STATUS.recovery | STATUS.fatal | STATUS.off)) ||
      busy();
  }

  function busy() {
    return initialized && !failed && Boolean(module._sim_busy());
  }

  function isDirty() {
    return initialized && (staged || module._sim_revision() !== savedRevision);
  }

  function readStored(nextProfile) {
    let encoded;
    try {
      encoded = localStorage.getItem(STORAGE_PREFIX + nextProfile);
    } catch (error) {
      return { bytes: new Uint8Array(EEPROM_SIZE).fill(255),
        warning: `Browser-Speicher nicht lesbar: ${error.message}. ` +
          "Diese Sitzung startet mit leerem EEPROM. Unbedingt eine Datei als Backup herunterladen.",
        protect: true };
    }
    if (encoded === null) return { bytes: new Uint8Array(EEPROM_SIZE).fill(255), encoded: null };
    try {
      return { bytes: decodeEEPROM(encoded), encoded };
    } catch (error) {
      return { error: `${error.message} Der gespeicherte Inhalt wird nicht veraendert. ` +
        "Eine gueltige EEPROM-Datei importieren oder bewusst mit leerem EEPROM starten." };
    }
  }

  function snapshot() {
    if (!initialized) throw new Error("EEPROM ist noch nicht gestartet.");
    if (busy()) throw new Error("Firmware ist beschaeftigt oder ein Menue-Edit ist offen. Erst abschliessen.");
    return module.HEAPU8.slice(module._sim_eeprom(), module._sim_eeprom() + EEPROM_SIZE);
  }

  function loadControlSettings() {
    springs.fill(true);
    try {
      const encoded = localStorage.getItem(SETTINGS_PREFIX + profile);
      if (encoded !== null) {
        const values = JSON.parse(encoded);
        if (!Array.isArray(values) || values.length !== 4 ||
          values.some(value => typeof value !== "boolean")) {
          throw new Error("Ungueltige Rueckstelloptionen");
        }
        springs.splice(0, 4, ...values);
      }
    } catch (error) {
      notice(`Geberoptionen nicht lesbar: ${error.message}. Standard-Rueckstellung ist aktiv.`, true);
    }
    springInputs.forEach((input, index) => { input.checked = springs[index]; });
  }

  function save(now, explicit = false) {
    if (!initialized || failed || changing || busy()) return false;
    if (!explicit && (staged || protectedStorage || (flags & (STATUS.recovery | STATUS.fatal)))) return false;
    const revision = module._sim_revision();
    if (!explicit && revision === savedRevision) return true;
    try {
      if (!explicit && localStorage.getItem(STORAGE_PREFIX + profile) !== savedEncoded) {
        protectedStorage = true;
        storageWarning("Der Browser-Stand wurde in einem anderen Fenster geaendert. " +
          "Autosicherung angehalten. Laufendes EEPROM herunterladen oder Browser-Stand neu laden.");
        return false;
      }
      const encoded = encodeEEPROM(snapshot());
      localStorage.setItem(STORAGE_PREFIX + profile, encoded);
      savedEncoded = encoded;
      savedRevision = revision;
      staged = false;
      protectedStorage = false;
      saveRetryAt = 0;
      storageWarning("");
      detail(`Lokal gespeichert (${new Date().toLocaleTimeString("de-DE")}). Kein Datei-Backup.`);
      updateEnabled();
      return true;
    } catch (error) {
      saveRetryAt = now + 10000;
      storageWarning(`Automatisches Speichern fehlgeschlagen: ${error.message}. ` +
        "Aenderungen sind nur in dieser Sitzung. Bitte EEPROM herunterladen; Browser-Stand wurde nicht ersetzt.");
      return false;
    }
  }

  function startEEPROM(bytes, asStaged = false) {
    stopFrames();
    releaseAll();
    initialized = false;
    failed = false;
    elements.fatal.hidden = true;
    flags = 0;
    module.HEAPU8.set(bytes, module._sim_eeprom());
    module._sim_init();
    initialized = true;
    staged = asStaged;
    savedRevision = observedRevision = module._sim_revision();
    changedAt = performance.now();
    saveRetryAt = 0;
    for (let i = 0; i < analogCount; i++) module._sim_set_analog(i, analogs[i]);
    for (let i = 0; i < switches.length; i++) module._sim_set_switch(i, switches[i]);
    digitalModes = -1;
    module._sim_step(1);
    flags = module._sim_status();
    if (asStaged) detail("Teststand: Bisheriger Browser-Stand bleibt unveraendert. Erst nach Pruefung bewusst uebernehmen.");
    else detail("Browser-Speicher aktiv. Aenderungen werden nach Abschluss aller Schreibvorgaenge gesichert.");
    updateEnabled();
    render();
    startFrames();
  }

  async function changeProfile(nextProfile, first = false) {
    elements.profile.value = profile;
    if (changing || (!first && nextProfile === profile)) return;
    if (!first && !allowDiscard("Senderprofil wechseln")) return;
    changing = true;
    releaseAll();
    stopFrames();
    updateEnabled();
    notice(`Lade FS-${nextProfile.slice(0, 2)}${nextProfile.slice(2).toUpperCase()} WebAssembly ...`);
    try {
      const { default: createCoreTX } = await import(`./coretx-${nextProfile}.js`);
      const nextModule = await createCoreTX();
      for (const name of ["_sim_init", "_sim_step", "_sim_analog_count", "_sim_set_analog", "_sim_set_switch", "_sim_digital_inputs",
        "_sim_key", "_sim_trim", "_sim_touch", "_sim_eeprom", "_sim_revision", "_sim_busy",
        "_sim_status", "_sim_lcd", "_sim_channels", "_sim_error", "_sim_output", "UTF8ToString"]) {
        if (typeof nextModule[name] !== "function") throw new Error(`WASM-Export fehlt: ${name}`);
      }
      const stored = readStored(nextProfile);
      module = nextModule;
      profile = nextProfile;
      analogCount = Math.min(module._sim_analog_count(), AXES.length);
      initialized = false;
      failed = false;
      protectedStorage = Boolean(stored.protect || stored.error);
      savedEncoded = stored.encoded ?? null;
      staged = false;
      switches.fill(0);
      analogs.fill(2048);
      elements.profile.value = profile;
      elements["lcd-hint"].textContent = profile === "i6s"
        ? "Touch-LCD: Antippen, Wischen und Halten wie am Sender; Zusatzknoepfe beruehren die LCD-Touchleisten."
        : "LCD der nativen Firmware; Bedienung mit den Sendertasten.";
      elements["control-hint"].textContent = profile === "i6s"
        ? "LCD-Touchleisten: Oben, OK und Zurueck. OK halten oeffnet das Sendermenue. " +
          "Tastatur: Pfeil hoch / Enter / Escape beruehren dieselben LCD-Flaechen."
        : "Sendertasten: Hoch, Runter, OK und Zurueck. OK oder Zurueck halten fuer Langdruck. " +
          "Tastatur: Pfeil hoch / runter, Enter / Escape; ebenfalls halten statt zusaetzlicher Langdruck-Tasten.";
      elements.lcd.setAttribute("aria-label", profile === "i6s"
        ? "Touch-LCD, 128 mal 64 Pixel. Pfeil hoch, Enter und Escape bedienen die LCD-Touchleisten."
        : "Sender-LCD, 128 mal 64 Pixel. Pfeil hoch und runter, Enter fuer OK, Escape fuer Zurueck.");
      elements.lcd.classList.toggle("touch", profile === "i6s");
      buildPhysicalControls();
      storageWarning(stored.warning || stored.error || "");
      elements.fatal.hidden = true;
      changing = false;
      notice("");
      loadControlSettings();
      if (stored.error) {
        elements["run-status"].textContent = "Browser-EEPROM beschaedigt: Start gesperrt";
        detail("Keine Daten wurden verworfen. Importieren oder leeren Teststand starten.");
        context?.clearRect(0, 0, 128, 64);
        renderChannels();
      } else {
        startEEPROM(stored.bytes);
      }
    } catch (error) {
      changing = false;
      notice(`Profil konnte nicht geladen werden: ${error.message}. ` +
        "Die passenden .js- und .wasm-Dateien muessen neben dieser Seite liegen. Ueber HTTP(S) oeffnen.", true);
      if (initialized) startFrames();
      else elements["run-status"].textContent = "Firmware nicht verfuegbar";
    }
    updateEnabled();
  }

  function allowDiscard(action) {
    if (!initialized) return true;
    const unsafe = busy();
    const dirty = isDirty();
    if (!unsafe && !staged && dirty && !protectedStorage) save(performance.now());
    if (unsafe || staged || isDirty() || persistentWarning) {
      return window.confirm(`${action}?\n\n` +
        (unsafe ? "Die Firmware hat offene Menue-Aenderungen oder schreibt gerade. " : "") +
        "Nicht gespeicherte Aenderungen und ein nicht uebernommener Teststand gehen verloren. " +
        "Bei Abbrechen erst Menue-Edit abschliessen und EEPROM herunterladen.");
    }
    return true;
  }

  function setAnalog(index, value) {
    if (!Number.isFinite(value)) return;
    analogs[index] = clamp(Math.round(value), 0, 4095);
    const view = axisViews[index];
    if (view) {
      view.range.value = analogs[index];
      view.number.value = analogs[index];
    }
    updateStickPositions();
    potViews[index]?.update(analogs[index]);
    if (index < analogCount) nativeCall(() => module._sim_set_analog(index, analogs[index]));
  }

  function updateStickPositions() {
    for (const view of stickViews) {
      const x = analogs[view.x] / 4095;
      const y = analogs[view.y] / 4095;
      view.knob.style.left = `${(view.right ? 1 - x : x) * 100}%`;
      view.knob.style.top = `${(view.right ? y : 1 - y) * 100}%`;
    }
  }

  function setHold(id, source, down, setter) {
    let hold = holds.get(id);
    if (!hold) {
      if (!down) return;
      hold = { sources: new Set(), setter };
      holds.set(id, hold);
    }
    const before = hold.sources.size;
    if (down) hold.sources.add(source);
    else hold.sources.delete(source);
    if (!before && hold.sources.size) nativeCall(() => setter(true));
    if (before && !hold.sources.size) {
      nativeCall(() => setter(false));
      holds.delete(id);
    }
  }

  function bindHold(button, id, setter, dynamic = false) {
    const pointerSource = `${id}:pointer`;
    const keyboardSource = `${id}:button`;
    let activePointer = null;
    const release = () => {
      if (activePointer !== null) {
        activePointer = null;
        setHold(id, pointerSource, false, setter);
      }
      button.classList.remove("held");
    };
    pointerReleases.add(release);
    if (dynamic) dynamicReleases.add(release);
    button.addEventListener("pointerdown", event => {
      if (event.button !== 0 || activePointer !== null) return;
      event.preventDefault();
      button.focus({ preventScroll: true });
      activePointer = event.pointerId;
      button.setPointerCapture(event.pointerId);
      setHold(id, pointerSource, true, setter);
      button.classList.add("held");
    });
    for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) {
      button.addEventListener(type, event => {
        if (activePointer === event.pointerId) release();
      });
    }
    button.addEventListener("keydown", event => {
      if (!["Enter", " "].includes(event.key)) return;
      event.preventDefault();
      if (!event.repeat) {
        setHold(id, keyboardSource, true, setter);
        button.classList.add("held");
      }
    });
    button.addEventListener("keyup", event => {
      if (!["Enter", " "].includes(event.key)) return;
      event.preventDefault();
      setHold(id, keyboardSource, false, setter);
      button.classList.remove("held");
    });
    button.addEventListener("blur", () => {
      setHold(id, keyboardSource, false, setter);
      button.classList.remove("held");
    });
    button.addEventListener("click", event => {
      // Assistive technology may activate a button without pointer or keyboard events.
      if (event.detail === 0 && !holds.has(id)) {
        const source = `${id}:activation`;
        setHold(id, source, true, setter);
        window.setTimeout(() => setHold(id, source, false, setter), 90);
      }
    });
    return release;
  }

  function buildAxes() {
    AXES.forEach(([name, description], index) => {
      if (index < PANEL_AXES) {
        axisViews.push(null);
        return;
      }
      const row = document.createElement("div");
      row.className = "axis-control";
      const label = document.createElement("label");
      label.htmlFor = `axis-${index}`;
      label.append(document.createTextNode(name));
      const sub = document.createElement("small");
      sub.textContent = description;
      label.append(sub);
      const range = document.createElement("input");
      range.type = "range";
      range.min = "0";
      range.max = "4095";
      range.step = "1";
      range.id = `axis-${index}`;
      range.value = "2048";
      range.addEventListener("input", () => setAnalog(index, range.valueAsNumber));
      const number = document.createElement("input");
      number.type = "number";
      number.min = "0";
      number.max = "4095";
      number.step = "1";
      number.value = "2048";
      number.setAttribute("aria-label", `${name} ADC-Rohwert`);
      number.addEventListener("input", () => {
        if (number.value !== "" && number.validity.valid) setAnalog(index, number.valueAsNumber);
      });
      number.addEventListener("change", () => setAnalog(index,
        Number.isFinite(number.valueAsNumber) ? number.valueAsNumber : analogs[index]));
      number.addEventListener("blur", () => { number.value = analogs[index]; });
      row.append(label, range, number);
      elements.axes.append(row);
      axisViews.push({ row, range, number });
    });
  }

  function buildSticks() {
    for (const right of [false, true]) {
      const x = right ? 0 : 3;
      const y = right ? 1 : 2;
      const wrapper = document.createElement("div");
      wrapper.className = "stick-control";
      const label = document.createElement("p");
      label.className = "stick-label";
      label.id = right ? "right-stick-label" : "left-stick-label";
      label.textContent = right ? "Rechts / RH + RV" : "Links / LH + LV";
      const pad = document.createElement("div");
      pad.className = "stick-pad";
      pad.setAttribute("role", "group");
      pad.setAttribute("aria-labelledby", label.id);
      pad.setAttribute("aria-description", "Ziehen mit Maus oder Touch.");
      const knob = document.createElement("span");
      knob.className = "stick-knob";
      pad.append(knob);
      let pointer = null;
      const move = event => {
        const [rawX, rawY] = stickCoordinates(event.clientX, event.clientY, pad.getBoundingClientRect(), right);
        setAnalog(x, rawX);
        setAnalog(y, rawY);
      };
      const release = () => {
        if (pointer === null) return;
        pointer = null;
        if (springs[x]) setAnalog(x, 2048);
        if (springs[y]) setAnalog(y, 2048);
      };
      pointerReleases.add(release);
      pad.addEventListener("pointerdown", event => {
        if (event.button !== 0 || pointer !== null || !initialized || changing || failed) return;
        pointer = event.pointerId;
        pad.setPointerCapture(pointer);
        event.preventDefault();
        move(event);
      });
      pad.addEventListener("pointermove", event => { if (pointer === event.pointerId) move(event); });
      for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) {
        pad.addEventListener(type, event => { if (pointer === event.pointerId) release(); });
      }
      for (const index of [x, y]) {
        const toggleLabel = document.createElement("label");
        const input = document.createElement("input");
        input.type = "checkbox";
        input.checked = springs[index];
        springInputs[index] = input;
        input.setAttribute("aria-label", `${AXES[index][0]} automatisch zentrieren`);
        input.addEventListener("change", () => {
          springs[index] = input.checked;
          if (input.checked) setAnalog(index, 2048);
          try {
            localStorage.setItem(SETTINGS_PREFIX + profile, JSON.stringify(springs));
          } catch (error) {
            notice(`Geberoptionen nicht gespeichert: ${error.message}.`, true);
          }
        });
        toggleLabel.append(input, document.createTextNode(AXES[index][0]));
        elements["stick-centering"].append(toggleLabel);
      }
      wrapper.append(label, pad);
      if (right) elements.sticks.append(wrapper);
      else elements.sticks.insertBefore(wrapper, elements["stick-centering"].parentElement);
      stickViews.push({ x, y, knob, right });
    }
  }

  function buildSwitch(name, id, threeWay, initial, change) {
    const label = document.createElement("div");
    label.className = "switch-control";
    const caption = document.createElement("span");
    caption.textContent = `${name} / ${threeWay ? 3 : 2}-fach`;
    const button = document.createElement("button");
    button.type = "button";
    button.className = "switch-toggle";
    button.dataset.position = String(initial);
    button.setAttribute("aria-label", `Schalter ${name}`);
    const lever = document.createElement("span");
    lever.className = "switch-lever";
    lever.setAttribute("aria-hidden", "true");
    button.append(lever);
    const position = document.createElement("span");
    position.className = "switch-position";
    position.id = `switch-position-${id}`;
    position.textContent = ["Oben", "Mitte", "Unten"][initial];
    position.setAttribute("aria-live", "polite");
    button.setAttribute("aria-describedby", position.id);
    button.title = `${threeWay ? 3 : 2} Stellungen; mit Klick, Enter oder Leertaste umschalten.`;
    const positions = threeWay ? [0, 1, 2] : [0, 2];
    let current = initial;
    button.addEventListener("click", () => {
      current = positions[(positions.indexOf(current) + 1) % positions.length];
      button.dataset.position = String(current);
      position.textContent = ["Oben", "Mitte", "Unten"][current];
      change(current);
    });
    label.append(caption, button, position);
    return label;
  }

  // Digital extension inputs appear as soon as they are enabled in the sender input setup.
  // Two-position: contact closed = down. Three-position: contact i = up, contact i+1 = down.
  function updateDigitalSwitches() {
    const modes = module._sim_digital_inputs();
    if (modes === digitalModes || !digitalSwitches) return;
    digitalModes = modes;
    digitalSwitches.replaceChildren();
    for (const { id, release } of digitalPushes) {
      release();
      pointerReleases.delete(release);
      dynamicReleases.delete(release);
      holds.delete(id);
    }
    digitalPushes = [];
    if (profile === "i6x") edges.forEach(({ buttons }) => buttons.replaceChildren());
    for (let i = 0; i < DIGITAL_INPUTS; i++) module._sim_set_switch(4 + i, 0);
    DIGITAL_NAMES[profile].forEach((name, i) => {
      const mode = (modes >> (2 * i)) & 3;
      if (!mode) return;
      const threeWay = mode === 2;
      if (profile === "i6x") {
        // i6X: SE at the left, SF at the right edge. Three-position: upper and lower contact.
        const contacts = threeWay ? [[i, "oben"], [i + 1, "unten"]] : [[i, ""]];
        for (const [bit, text] of contacts) {
          const id = `d${bit}`;
          const push = buildPushButton(name, id, text, down => module._sim_set_switch(4 + bit, down ? 2 : 0));
          digitalPushes.push({ id, release: push.release });
          edges[Math.min(i, 1)].buttons.append(push.wrapper);
        }
        return;
      }
      const apply = position => nativeCall(() => {
        if (threeWay) {
          module._sim_set_switch(4 + i, position === 0 ? 2 : 0);
          module._sim_set_switch(5 + i, position === 2 ? 2 : 0);
        } else module._sim_set_switch(4 + i, position);
      });
      digitalSwitches.append(buildSwitch(name, 4 + i, threeWay, threeWay ? 1 : 0, apply));
    });
  }

  // i6X: VRA/VRB are free-running rotary pots between the sticks; i6S: self-centering
  // side sliders next to the sticks.
  function buildPot(index, slider) {
    const [name] = AXES[index];
    const wrapper = document.createElement("div");
    wrapper.className = slider ? "pot-control slider" : "pot-control knob";
    const control = document.createElement("div");
    control.className = slider ? "pot-slider" : "pot-knob";
    control.tabIndex = 0;
    control.setAttribute("role", "slider");
    control.setAttribute("aria-label", slider ? `${name} Slider, selbstneutralisierend` : `${name} Drehpoti`);
    control.setAttribute("aria-orientation", "vertical");
    control.setAttribute("aria-valuemin", "0");
    control.setAttribute("aria-valuemax", "4095");
    control.title = slider ? `${name}: ziehen, federt auf Mitte zurueck`
      : `${name}: senkrecht ziehen, Mausrad oder Pfeiltasten; Doppelklick stellt Mitte ein`;
    const thumb = document.createElement("span");
    thumb.className = slider ? "pot-thumb" : "pot-marker";
    control.append(thumb);
    const caption = document.createElement("span");
    caption.className = "pot-label";
    caption.textContent = name;
    wrapper.append(control, caption);
    const active = () => initialized && !changing && !failed;
    let pointer = null;
    let startY = 0;
    let startValue = 2048;
    const fromPointer = event => {
      if (slider) {
        const rect = control.getBoundingClientRect();
        return (1 - clamp((event.clientY - rect.top) / rect.height, 0, 1)) * 4095;
      }
      return startValue + (startY - event.clientY) * 20;
    };
    const release = () => {
      if (pointer === null) return;
      pointer = null;
      control.classList.remove("held");
      if (slider) setAnalog(index, 2048);
    };
    pointerReleases.add(release);
    dynamicReleases.add(release);
    control.addEventListener("pointerdown", event => {
      if (event.button !== 0 || pointer !== null || !active()) return;
      event.preventDefault();
      control.focus({ preventScroll: true });
      pointer = event.pointerId;
      startY = event.clientY;
      startValue = analogs[index];
      control.setPointerCapture(pointer);
      control.classList.add("held");
      if (slider) setAnalog(index, fromPointer(event));
    });
    control.addEventListener("pointermove", event => {
      if (pointer === event.pointerId) setAnalog(index, fromPointer(event));
    });
    for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) {
      control.addEventListener(type, event => { if (pointer === event.pointerId) release(); });
    }
    control.addEventListener("keydown", event => {
      const step = event.shiftKey ? 512 : 64;
      const value = { ArrowUp: analogs[index] + step, ArrowRight: analogs[index] + step,
        ArrowDown: analogs[index] - step, ArrowLeft: analogs[index] - step,
        PageUp: analogs[index] + 512, PageDown: analogs[index] - 512,
        Home: 0, End: 4095 }[event.key];
      if (value === undefined || !active()) return;
      event.preventDefault();
      event.stopPropagation();
      setAnalog(index, value);
    });
    if (slider) {
      control.addEventListener("keyup", () => { if (pointer === null) setAnalog(index, 2048); });
      control.addEventListener("blur", () => { if (pointer === null) setAnalog(index, 2048); });
    } else {
      control.addEventListener("wheel", event => {
        if (!active() || event.deltaY === 0) return;
        event.preventDefault();
        setAnalog(index, analogs[index] + (event.deltaY < 0 ? 64 : -64));
      }, { passive: false });
      control.addEventListener("dblclick", () => { if (active()) setAnalog(index, 2048); });
    }
    potViews[index] = {
      update(value) {
        control.setAttribute("aria-valuenow", String(value));
        if (slider) thumb.style.top = `${(1 - value / 4095) * 100}%`;
        else thumb.style.transform = `rotate(${(value / 4095 - 0.5) * 270}deg)`;
      },
    };
    potViews[index].update(analogs[index]);
    return wrapper;
  }

  function buildPots() {
    document.querySelectorAll(".pot-control, .stick-edge").forEach(element => element.remove());
    potViews.length = 0;
    const slider = profile === "i6s";
    // Outer columns of the stick row: SE/SF push buttons and the i6S side sliders.
    edges = ["left", "right"].map(side => {
      const edge = document.createElement("div");
      edge.className = `stick-edge ${side}`;
      const buttons = document.createElement("div");
      buttons.className = "edge-buttons";
      edge.append(buttons);
      return { edge, buttons };
    });
    elements.sticks.prepend(edges[0].edge);
    elements.sticks.append(edges[1].edge);
    const [vra, vrb] = [buildPot(4, slider), buildPot(5, slider)];
    if (slider) {
      edges[0].edge.append(vra);
      edges[1].edge.append(vrb);
    } else
      elements.pots.append(vra, vrb);
  }

  function buildPushButton(name, id, text, setter) {
    const wrapper = document.createElement("div");
    wrapper.className = "push-control";
    const button = document.createElement("button");
    button.type = "button";
    button.className = "push-button";
    button.setAttribute("aria-label", `Taster ${name}${text ? ` ${text}` : ""}, halten zum Druecken`);
    button.title = `${name}${text ? ` ${text}` : ""}: Taster, gedrueckt halten`;
    const caption = document.createElement("span");
    caption.className = "push-label";
    caption.textContent = text ? `${name} ${text}` : name;
    wrapper.append(caption, button);
    return { wrapper, release: bindHold(button, id, setter, true) };
  }

  function buildPhysicalControls() {
    for (const release of dynamicReleases) {
      release();
      pointerReleases.delete(release);
    }
    dynamicReleases.clear();
    elements.switches.replaceChildren();
    buildPots();
    axisViews.forEach((view, index) => { if (view) view.row.hidden = index >= analogCount; });
    elements["trims-left"].replaceChildren();
    elements["trims-right"].replaceChildren();
    elements["trims-left"].hidden = profile === "i6s";
    elements["trims-right"].hidden = profile === "i6s";
    elements["lcd-controls"].classList.toggle("has-trims", profile === "i6x");
    elements.navigation.replaceChildren();
    navigationKeys.clear();
    elements.navigation.classList.toggle("touch-controls", profile === "i6s");
    elements.navigation.setAttribute("aria-label", profile === "i6s" ? "LCD-Touchleisten" : "Sendertasten");
    const controls = profile === "i6s" ? [
      { label: "LCD: Oben", shortcut: "ArrowUp", touch: [64, 7] },
      { label: "LCD: OK", shortcut: "Enter", touch: [32, 55] },
      { label: "LCD: Zurueck", shortcut: "Escape", touch: [96, 55] },
    ] : [
      { label: "Hoch", shortcut: "ArrowUp", key: 1 },
      { label: "Runter", shortcut: "ArrowDown", key: 2 },
      { label: "OK", shortcut: "Enter", key: 5 },
      { label: "Zurueck", shortcut: "Escape", key: 6 },
    ];
    for (const control of controls) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = control.label;
      button.classList.toggle("primary", control.shortcut === "Enter");
      const id = control.key ? `k${control.key}` : `touch:${control.touch.join(",")}`;
      let setter;
      if (control.key) {
        button.dataset.key = control.key;
        setter = down => module._sim_key(control.key, Number(down));
      } else {
        const [x, y] = control.touch;
        button.dataset.touch = control.touch.join(",");
        setter = down => {
          if (down) {
            if (touch) return;
            touch = { id, x, y };
            module._sim_touch(x, y, 1);
          } else if (touch?.id === id) endTouch();
        };
        for (const type of ["pointercancel", "lostpointercapture", "blur"]) {
          button.addEventListener(type, () => {
            if (touch?.id === id) endTouch(true);
          });
        }
      }
      navigationKeys.set(control.shortcut, { id, setter });
      bindHold(button, id, setter, true);
      elements.navigation.append(button);
    }
    ["SA", "SB", "SC", "SD"].forEach((name, index) => {
      const threeWay = index === 2 || (index === 1 && profile === "i6s");
      elements.switches.append(buildSwitch(name, index, threeWay, 0, position => {
        switches[index] = position;
        nativeCall(() => module._sim_set_switch(index, position));
      }));
    });
    digitalSwitches = document.createElement("div");
    digitalSwitches.className = "digital-switches";
    elements.switches.append(digitalSwitches);
    digitalModes = -1;
    if (profile === "i6s") {
      [["SE", 14], ["SF", 15]].forEach(([name, index], side) => {
        edges[side].buttons.append(buildPushButton(name, `s${index}`, "", down => {
          switches[index] = down ? 2 : 0;
          module._sim_set_switch(index, switches[index]);
        }).wrapper);
      });
    } else {
      AXES.slice(0, 4).forEach(([name], axis) => {
        const pair = document.createElement("div");
        pair.className = "trim-pair";
        const label = document.createElement("span");
        label.textContent = name;
        pair.append(label);
        [["+", 0], ["-", 1]].forEach(([text, offset]) => {
          const button = document.createElement("button");
          button.type = "button";
          button.textContent = text;
          button.setAttribute("aria-label", `Trimmung ${name} ${text === "+" ? "erhoehen" : "verringern"}`);
          const index = axis * 2 + offset;
          bindHold(button, `t${index}`, down => module._sim_trim(index, Number(down)), true);
          pair.append(button);
        });
        elements[axis < 2 ? "trims-right" : "trims-left"].append(pair);
      });
    }
    for (let i = 0; i < analogs.length; i++) setAnalog(i, analogs[i]);
    updateStickPositions();
    resizeLCD();
  }

  function buildChannels() {
    for (let i = 0; i < 16; i++) {
      const row = document.createElement("div");
      row.className = "channel";
      const label = document.createElement("span");
      label.className = "channel-label";
      label.textContent = `CH${String(i + 1).padStart(2, "0")}`;
      const track = document.createElement("div");
      track.className = "channel-track";
      track.setAttribute("aria-hidden", "true");
      const fill = document.createElement("span");
      fill.className = "channel-fill";
      track.append(fill);
      const value = document.createElement("span");
      value.className = "channel-value";
      value.textContent = "--";
      row.append(label, track, value);
      elements.channels.append(row);
      channelViews.push({ row, fill, value });
    }
  }

  function renderChannels() {
    const valid = initialized && !failed && Boolean(flags & STATUS.valid) &&
      !(flags & (STATUS.off | STATUS.fatal | STATUS.recovery));
    elements.channels.classList.toggle("invalid", !valid);
    elements["output-state"].textContent = valid
      ? "Gueltige Firmware-Ausgaenge / kein RF-Signal"
      : "UNGUELTIG / keine verlaesslichen Kanalausgaenge";
    const base = valid ? module._sim_channels() >> 1 : 0;
    channelViews.forEach((view, index) => {
      if (!valid) {
        if (view.value.textContent !== "--") view.value.textContent = "--";
        view.row.classList.remove("overtravel");
        return;
      }
      const { percent, left, width } = channelPresentation(module.HEAP16[base + index]);
      const text = `${percent > 0 ? "+" : ""}${percent.toFixed(1)}%`;
      if (view.value.textContent !== text) view.value.textContent = text;
      view.fill.style.left = `${left}%`;
      view.fill.style.width = `${width}%`;
      view.row.classList.toggle("overtravel", Math.abs(percent) > 100);
    });
  }

  function render() {
    if (!initialized) return;
    flags = module._sim_status();
    if (context && image) {
      const base = module._sim_lcd();
      for (let i = 0; i < 8192; i++) {
        const on = module.HEAPU8[base + i] !== 0;
        const offset = i * 4;
        image.data[offset] = on ? 30 : 182;
        image.data[offset + 1] = on ? 48 : 201;
        image.data[offset + 2] = on ? 32 : 155;
        image.data[offset + 3] = 255;
      }
      context.putImageData(image, 0, 0);
    }
    renderChannels();
    updateDigitalSwitches();
    let state;
    if (flags & STATUS.fatal) state = "Fataler Firmware-Fehler";
    else if (flags & STATUS.off) state = "Sender ausgeschaltet";
    else if (flags & STATUS.recovery) state = "Wiederherstellung erforderlich";
    else if (!(flags & STATUS.loaded)) state = "Einrichtung / Kalibrierung";
    else if (!(flags & STATUS.editable)) state = "Modelle geladen / schreibgeschuetzt";
    else state = "Modelle geladen / bereit";
    const message = module.UTF8ToString(module._sim_error());
    if (message) state += ` / ${message}`;
    if (elements["run-status"].textContent !== state) elements["run-status"].textContent = state;
    elements["run-status"].dataset.warning = String(!(flags & STATUS.valid));
    const isBusy = busy();
    const dirty = isDirty();
    let storageState = isBusy ? "Firmware beschaeftigt / Menue-Edit offen"
      : staged ? "Teststand / nicht im Browser uebernommen"
      : protectedStorage ? "Browser-Stand geschuetzt / keine Autosicherung"
      : persistentWarning ? "Speicherfehler / bitte Datei sichern"
      : dirty ? "Aenderungen / Sicherung ausstehend" : "Browser-Stand gesichert";
    if (flags & STATUS.recovery) storageState = "Wiederherstellung / Autosicherung gesperrt";
    elements["storage-status"].textContent = storageState;
    elements["storage-status"].dataset.warning = String(isBusy || dirty || Boolean(persistentWarning));
    const output = module._sim_output();
    elements["rf-status"].textContent = `RF simuliert: ${output === 1 ? "intern" : output === 2 ? "extern" : "AUS"} / kein Funk`;
    updateEnabled();
  }

  function tick(now) {
    frame = null;
    if (document.hidden || changing || !initialized || failed) return;
    try {
      const elapsed = lastFrame === null ? 0 : clamp(Math.round(now - lastFrame), 0, 50);
      lastFrame = now;
      if (elapsed) module._sim_step(elapsed);
      render();
      const revision = module._sim_revision();
      if (revision !== observedRevision) {
        observedRevision = revision;
        changedAt = now;
      }
      if (revision !== savedRevision && now - changedAt >= 800 && now >= saveRetryAt) save(now);
    } catch (error) {
      fatal(error);
      return;
    }
    frame = requestAnimationFrame(tick);
  }

  function startFrames() {
    lastFrame = null;
    if (frame === null && initialized && !changing && !document.hidden && !failed) {
      frame = requestAnimationFrame(tick);
    }
  }

  function stopFrames() {
    if (frame !== null) cancelAnimationFrame(frame);
    frame = null;
    lastFrame = null;
  }

  function endTouch(cancel = false) {
    if (!touch) return;
    const previous = touch;
    touch = null;
    nativeCall(() => module._sim_touch(previous.x, previous.y, cancel ? -1 : 0));
  }

  function releaseAll() {
    endTouch(true);
    for (const release of pointerReleases) release();
    for (const hold of holds.values()) {
      // Avoid nativeCall recursion while recovering from a trap.
      if (initialized && module && !failed) {
        try { hold.setter(false); } catch (error) {
          storageWarning(`Freigabe der Tasten fehlgeschlagen: ${error.message}. Profil bitte neu laden.`);
        }
      }
    }
    holds.clear();
    keyboardHolds.clear();
    document.querySelectorAll("button.held").forEach(button => button.classList.remove("held"));
    if (axisViews.length === AXES.length) {
      for (let i = 0; i < STICK_AXES; i++) if (springs[i]) setAnalog(i, 2048);
    }
  }

  function isFormEdit(target) {
    return target instanceof Element &&
      Boolean(target.closest("input, textarea, select, button, summary, a, [contenteditable]:not([contenteditable='false'])"));
  }

  document.addEventListener("keydown", event => {
    if (event.repeat || event.isComposing || event.altKey || event.ctrlKey || event.metaKey ||
      isFormEdit(event.target) || !initialized || failed || changing) return;
    const control = navigationKeys.get(event.key);
    if (!control) return;
    event.preventDefault();
    keyboardHolds.set(event.code, control);
    setHold(control.id, `keyboard:${event.code}`, true, control.setter);
  });
  document.addEventListener("keyup", event => {
    const control = keyboardHolds.get(event.code);
    if (!control) return;
    event.preventDefault();
    keyboardHolds.delete(event.code);
    setHold(control.id, `keyboard:${event.code}`, false, control.setter);
  });

  elements.lcd.addEventListener("pointerdown", event => {
    if (profile !== "i6s" || !initialized || changing || failed || event.button !== 0 || touch) return;
    event.preventDefault();
    elements.lcd.focus({ preventScroll: true });
    const [x, y] = lcdPoint(event.clientX, event.clientY, elements.lcd.getBoundingClientRect());
    touch = { id: event.pointerId, x, y };
    elements.lcd.setPointerCapture(event.pointerId);
    nativeCall(() => module._sim_touch(x, y, 1));
  });
  elements.lcd.addEventListener("pointermove", event => {
    if (touch?.id !== event.pointerId) return;
    const [x, y] = lcdPoint(event.clientX, event.clientY, elements.lcd.getBoundingClientRect());
    touch.x = x;
    touch.y = y;
    nativeCall(() => module._sim_touch(x, y, 1));
  });
  for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) {
    elements.lcd.addEventListener(type, event => {
      if (touch?.id === event.pointerId) endTouch(type !== "pointerup");
    });
  }

  elements["expert-mode"].addEventListener("change", () => {
    const expert = elements["expert-mode"].checked;
    elements["analog-panel"].hidden = !expert;
    elements["storage-panel"].hidden = !expert;
    document.body.classList.toggle("expert-mode", expert);
  });
  elements.profile.addEventListener("change", () => changeProfile(elements.profile.value));
  elements.download.addEventListener("click", () => {
    try {
      const bytes = snapshot();
      const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
      const link = document.createElement("a");
      link.href = url;
      link.download = `coretx-${profile}-${new Date().toISOString().replace(/[:.]/g, "-")}.bin`;
      document.body.append(link);
      link.click();
      link.remove();
      window.setTimeout(() => URL.revokeObjectURL(url), 1000);
      notice("EEPROM-Download angefordert. Datei im Download-Ordner pruefen; Browser-Speicher bleibt unveraendert.");
    } catch (error) {
      notice(`Kein Download: ${error.message}`, true);
    }
  });

  elements["import-file"].addEventListener("change", async () => {
    const file = elements["import-file"].files[0];
    elements["import-file"].value = "";
    if (!file || !module || changing) return;
    if (file.size !== EEPROM_SIZE) {
      notice(`Import abgelehnt: ${file.size} Bytes statt genau 16384 Bytes. Aktueller Stand bleibt unveraendert.`, true);
      return;
    }
    const importingProfile = profile;
    let bytes;
    try { bytes = new Uint8Array(await file.arrayBuffer()); } catch (error) {
      notice(`Datei konnte nicht gelesen werden: ${error.message}`, true);
      return;
    }
    if (profile !== importingProfile || changing) {
      notice("Import abgebrochen, weil das Senderprofil inzwischen gewechselt wurde.", true);
      return;
    }
    if (!window.confirm(`EEPROM-Datei "${file.name}" als Teststand fuer ${profile.toUpperCase()} laden?\n\n` +
      "Dies ersetzt den laufenden Senderzustand, einschliesslich ungesicherter Aenderungen und offener Menue-Edits. " +
      "Der bisher gespeicherte Browser-Stand bleibt erhalten. Die Firmware prueft das Format. " +
      "Erst 'Teststand im Browser uebernehmen' ersetzt die lokale Sicherung.")) return;
    try {
      startEEPROM(bytes, true);
      notice("Datei als Teststand geladen. LCD und nativen Status pruefen. Bei Wiederherstellung NICHT uebernehmen; " +
        "'Browser-Stand neu laden' stellt die vorherige lokale Sicherung wieder her.");
    } catch (error) { fatal(error); }
  });

  elements["commit-import"].addEventListener("click", () => {
    if (busy() || !(flags & STATUS.loaded) || (flags & (STATUS.recovery | STATUS.fatal | STATUS.off))) {
      notice("Teststand kann noch nicht uebernommen werden: Einrichtung abschliessen und nativen Fehlerstatus pruefen.", true);
      return;
    }
    if (!window.confirm("Teststand dauerhaft im Browser uebernehmen?\n\n" +
      `Dies ERSETZT die bisherige lokale EEPROM-Sicherung fuer ${profile.toUpperCase()}. ` +
      "Vorherige Modelle sind dann nur mit einem Datei-Backup wiederherstellbar.")) return;
    if (save(performance.now(), true)) notice("Teststand ist jetzt die lokale Browser-Sicherung.");
  });

  elements.reset.addEventListener("click", () => {
    if (!window.confirm("Simulator-Werkseinstellungen starten?\n\n" +
      "ALLE Modelle, Kalibrierungen und offenen Aenderungen der laufenden Sitzung gehen verloren. " +
      "Der Simulator erstellt ein Vierkanalmodell und passende Kalibrierwerte fuer seine virtuellen Geber. " +
      "Der bisherige Browser-Stand bleibt bis zum bewussten Uebernehmen des neuen Teststands erhalten.")) return;
    try {
      startEEPROM(new Uint8Array(EEPROM_SIZE).fill(255), true);
      notice("Neuer Vierkanal-Teststand mit kalibrierten virtuellen Gebern gestartet. " +
        "Anschliessend bewusst im Browser uebernehmen.");
    } catch (error) { fatal(error); }
  });

  elements.restore.addEventListener("click", () => {
    if (!allowDiscard("Gespeicherten Browser-Stand neu laden")) return;
    const stored = readStored(profile);
    if (stored.error) {
      storageWarning(stored.error);
      return;
    }
    if (stored.warning) {
      storageWarning(stored.warning);
      notice("Neu laden abgebrochen: Browser-Speicher nicht lesbar. Laufende Sitzung bleibt erhalten.", true);
      return;
    }
    try {
      protectedStorage = false;
      savedEncoded = stored.encoded ?? null;
      storageWarning("");
      startEEPROM(stored.bytes);
      notice("Gespeicherten Browser-Stand neu geladen.");
    } catch (error) { fatal(error); }
  });

  window.addEventListener("blur", releaseAll);
  window.addEventListener("storage", event => {
    if (event.storageArea === localStorage &&
      (event.key === STORAGE_PREFIX + profile || event.key === null)) {
      protectedStorage = true;
      storageWarning("Der Browser-Stand wurde in einem anderen Fenster geaendert. " +
        "Autosicherung angehalten. Laufendes EEPROM herunterladen oder Browser-Stand neu laden.");
    }
  });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      releaseAll();
      stopFrames();
      if (initialized && !busy() && isDirty()) save(performance.now());
    } else startFrames();
  });
  window.addEventListener("pagehide", () => { releaseAll(); stopFrames(); });
  window.addEventListener("pageshow", startFrames);
  window.addEventListener("beforeunload", event => {
    if (initialized && !busy() && !staged && !protectedStorage) save(performance.now());
    if ((initialized && (busy() || isDirty())) || persistentWarning) {
      event.preventDefault();
      event.returnValue = "";
    }
  });

  function resizeLCD() {
    const available = elements["lcd-frame"].parentElement.clientWidth - 26;
    const scale = clamp(Math.floor(available / 128), 1, window.innerWidth >= 1150 ? 3 : 2);
    elements.lcd.style.width = `${128 * scale}px`;
    elements.lcd.style.height = `${64 * scale}px`;
  }
  window.addEventListener("resize", resizeLCD);
  buildAxes();
  buildSticks();
  buildChannels();
  resizeLCD();
  if (!context) {
    elements.fatal.textContent = "Canvas 2D nicht verfuegbar. Bitte einen Browser mit Canvas-Unterstuetzung verwenden.";
    elements.fatal.hidden = false;
  } else changeProfile(profile, true);
}
