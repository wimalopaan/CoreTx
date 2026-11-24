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
  ["VRA", "Drehgeber / Seitengeber A"],
  ["VRB", "Drehgeber / Seitengeber B"],
  ["EXT 1", "Optional, ab Werk deaktiviert"],
  ["EXT 2", "Optional, ab Werk deaktiviert"],
];
const STATUS = { loaded: 1, editable: 2, valid: 4, busy: 8, recovery: 16, off: 32, fatal: 64 };
const STORAGE_PREFIX = "coretx.simulator.eeprom.v1.";
const SETTINGS_PREFIX = "coretx.simulator.controls.v1.";

if (typeof document !== "undefined") startBrowser();

function startBrowser() {
  const $ = id => document.getElementById(id);
  const elements = Object.fromEntries([
    "profile", "profile-badge", "native-controls", "analog-controls", "switches", "sticks",
    "trims", "axes", "lcd", "lcd-frame", "lcd-hint", "channels", "output-state", "run-status",
    "storage-status", "rf-status", "notice", "storage-warning", "fatal", "download",
    "import-file", "commit-import", "restore", "reset", "save-detail",
  ].map(id => [id, $(id)]));
  const context = elements.lcd.getContext("2d", { alpha: false });
  const image = context?.createImageData(128, 64);
  const analogs = new Uint16Array(8).fill(2048);
  const switches = new Uint8Array(16);
  const springs = [true, true, true, true];
  const springInputs = [];
  const axisViews = [];
  const stickViews = [];
  const channelViews = [];
  const holds = new Map();
  const keyboardHolds = new Map();
  const pointerReleases = new Set();
  const dynamicReleases = new Set();
  let module = null;
  let initialized = false;
  let profile = "i6x";
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
    for (let i = 0; i < analogs.length; i++) module._sim_set_analog(i, analogs[i]);
    for (let i = 0; i < switches.length; i++) module._sim_set_switch(i, switches[i]);
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
      for (const name of ["_sim_init", "_sim_step", "_sim_set_analog", "_sim_set_switch",
        "_sim_key", "_sim_trim", "_sim_touch", "_sim_eeprom", "_sim_revision", "_sim_busy",
        "_sim_status", "_sim_lcd", "_sim_channels", "_sim_error", "_sim_output", "UTF8ToString"]) {
        if (typeof nextModule[name] !== "function") throw new Error(`WASM-Export fehlt: ${name}`);
      }
      const stored = readStored(nextProfile);
      module = nextModule;
      profile = nextProfile;
      initialized = false;
      failed = false;
      protectedStorage = Boolean(stored.protect || stored.error);
      savedEncoded = stored.encoded ?? null;
      staged = false;
      switches.fill(0);
      analogs.fill(2048);
      elements.profile.value = profile;
      elements["profile-badge"].textContent = `FS-${profile.slice(0, 2)}${profile.slice(2).toUpperCase()}`;
      elements["lcd-hint"].textContent = profile === "i6s"
        ? "Touch-LCD: Antippen, Wischen und Halten wie am Sender; alternativ Sendertasten."
        : "LCD der nativen Firmware; Bedienung mit den Sendertasten.";
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
    axisViews[index].range.value = analogs[index];
    axisViews[index].number.value = analogs[index];
    updateStickPositions();
    nativeCall(() => module._sim_set_analog(index, analogs[index]));
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
  }

  function buildAxes() {
    AXES.forEach(([name, description], index) => {
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
      axisViews.push({ range, number });
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
      pad.setAttribute("aria-description", "Ziehen mit Maus oder Touch. Tastatur und genaue Werte unter Analoge Eingaenge.");
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
      const options = document.createElement("div");
      options.className = "spring-options";
      for (const [index, title] of [[x, "X"], [y, "Y"]]) {
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
        toggleLabel.append(input, document.createTextNode(`${title} zentrieren`));
        options.append(toggleLabel);
      }
      wrapper.append(label, pad, options);
      elements.sticks.append(wrapper);
      stickViews.push({ x, y, knob, right });
    }
  }

  function buildPhysicalControls() {
    for (const release of dynamicReleases) {
      release();
      pointerReleases.delete(release);
    }
    dynamicReleases.clear();
    elements.switches.replaceChildren();
    elements.trims.replaceChildren();
    ["SA", "SB", "SC", "SD"].forEach((name, index) => {
      const label = document.createElement("label");
      label.className = "switch-control";
      const threeWay = index === 2 || (index === 1 && profile === "i6s");
      label.append(document.createTextNode(`${name} / ${threeWay ? 3 : 2}-fach`));
      const select = document.createElement("select");
      select.setAttribute("aria-label", `Schalter ${name}`);
      for (const [value, text] of [[0, "Oben"], [1, "Mitte"], [2, "Unten"]]) {
        if (value === 1 && !threeWay) continue;
        const option = document.createElement("option");
        option.value = value;
        option.textContent = text;
        select.append(option);
      }
      select.addEventListener("change", () => {
        switches[index] = Number(select.value);
        nativeCall(() => module._sim_set_switch(index, switches[index]));
      });
      label.append(select);
      elements.switches.append(label);
    });
    if (profile === "i6s") {
      const rear = document.createElement("div");
      rear.className = "rear-controls";
      [["SE", 14], ["SF", 15]].forEach(([name, index]) => {
        const button = document.createElement("button");
        button.type = "button";
        button.textContent = `${name} / Ruecktaster halten`;
        bindHold(button, `s${index}`, down => {
          switches[index] = down ? 2 : 0;
          module._sim_set_switch(index, switches[index]);
        }, true);
        rear.append(button);
      });
      elements.switches.append(rear);
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
        elements.trims.append(pair);
      });
    }
    for (let i = 0; i < 8; i++) setAnalog(i, analogs[i]);
    updateStickPositions();
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
    endTouch(true);
    if (axisViews.length === 8) {
      for (let i = 0; i < 4; i++) if (springs[i]) setAnalog(i, 2048);
    }
  }

  function isFormEdit(target) {
    return target instanceof Element &&
      Boolean(target.closest("input, textarea, select, button, summary, a, [contenteditable]:not([contenteditable='false'])"));
  }

  document.addEventListener("keydown", event => {
    if (event.repeat || event.isComposing || event.altKey || event.ctrlKey || event.metaKey ||
      isFormEdit(event.target) || !initialized || failed || changing) return;
    const key = { ArrowUp: 1, ArrowDown: 2, ArrowLeft: 3, ArrowRight: 4,
      Enter: event.shiftKey ? 7 : 5, Escape: event.shiftKey ? 8 : 6 }[event.key];
    if (!key) return;
    event.preventDefault();
    keyboardHolds.set(event.code, key);
    setHold(`k${key}`, `keyboard:${event.code}`, true, down => module._sim_key(key, Number(down)));
  });
  document.addEventListener("keyup", event => {
    const key = keyboardHolds.get(event.code);
    if (!key) return;
    event.preventDefault();
    keyboardHolds.delete(event.code);
    setHold(`k${key}`, `keyboard:${event.code}`, false, down => module._sim_key(key, Number(down)));
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
  document.querySelectorAll("[data-key]").forEach(button => {
    const key = Number(button.dataset.key);
    bindHold(button, `k${key}`, down => module._sim_key(key, Number(down)));
  });
  resizeLCD();
  if (!context) {
    elements.fatal.textContent = "Canvas 2D nicht verfuegbar. Bitte einen Browser mit Canvas-Unterstuetzung verwenden.";
    elements.fatal.hidden = false;
  } else changeProfile(profile, true);
}
