import "./style.css";

(function () {
  const W = 1920,
    H = 1080;
  const canvas = document.getElementById("kvm");
  const kvmWrap = document.getElementById("kvm-wrap");
  const banner = document.getElementById("kvm-banner");
  const st = document.getElementById("st");
  const fpsDisplay = document.getElementById("fps-display");
  const statsOverlay = document.getElementById("stats-overlay");
  const btnModeAbs = document.getElementById("mode-abs");
  const btnModeRel = document.getElementById("mode-rel");
  const btnFullscreen = document.getElementById("btn-fullscreen");
  const btnSendEsc = document.getElementById("btn-send-esc");
  const btnCad = document.getElementById("btn-cad");
  const btnPasteClip = document.getElementById("btn-paste-clip");
  const atxTitle = document.getElementById("atx-title");
  const atxRow = document.getElementById("atx-row");
  const btnAtxPower = document.getElementById("btn-atx-power");
  const btnAtxReset = document.getElementById("btn-atx-reset");
  const btnAtxForce = document.getElementById("btn-atx-force");
  const ptrSensInput = document.getElementById("ptr-sens");
  const ptrSensVal = document.getElementById("ptr-sens-val");
  const jpegQInput = document.getElementById("jpeg-q");
  const btnJpegQ = document.getElementById("btn-jpeg-q");
  const showStatsInput = document.getElementById("show-stats");

  const PTR_SENS_KEY = "p4kvm_pointer_sensitivity_pct";
  const SHOW_STATS_KEY = "p4kvm_show_stats";
  const MODE_KEY = "p4kvm_pointer_mode";

  function lsGet(key) {
    try {
      return localStorage.getItem(key);
    } catch (e) {
      return null;
    }
  }
  function lsSet(key, value) {
    try {
      localStorage.setItem(key, value);
    } catch (e) {
      /* private mode */
    }
  }

  /* ------------------------------------------------------------------ */
  /* Pointer mode: "abs" (virtual tablet, default) or "rel" (pointer lock) */

  let mode = lsGet(MODE_KEY) === "rel" ? "rel" : "abs";

  function pointerLockActive() {
    return document.pointerLockElement === canvas;
  }

  function kbdCaptured() {
    if (mode === "rel") return pointerLockActive();
    return document.activeElement === canvas;
  }

  function updateModeUi() {
    btnModeAbs.classList.toggle("active", mode === "abs");
    btnModeRel.classList.toggle("active", mode === "rel");
    canvas.classList.toggle("kbd-captured", mode === "abs" && kbdCaptured());
    if (mode === "abs") {
      if (kbdCaptured()) {
        banner.classList.add("hidden");
        setStatus("Keyboard captured — click outside the video to release");
      } else {
        banner.textContent =
          "Tablet mode: move over the video to control the pointer, click it to also capture the keyboard.";
        banner.classList.remove("hidden");
        if (wsReady()) setStatus("Ready");
      }
    } else {
      if (pointerLockActive()) {
        banner.classList.add("hidden");
        setStatus("Locked (Esc releases)");
      } else {
        banner.textContent =
          "Relative mode: click the video to capture mouse and keyboard (pointer lock, Esc releases).";
        banner.classList.remove("hidden");
        if (wsReady()) setStatus("Ready");
      }
    }
  }

  function setMode(next) {
    if (next === mode) return;
    releaseAllKeys();
    if (pointerLockActive()) document.exitPointerLock();
    if (document.activeElement === canvas) canvas.blur();
    mode = next;
    lsSet(MODE_KEY, mode);
    updateModeUi();
  }

  btnModeAbs.addEventListener("click", function () {
    setMode("abs");
  });
  btnModeRel.addEventListener("click", function () {
    setMode("rel");
  });

  function setStatus(text) {
    st.textContent = text;
  }

  /* ------------------------------------------------------------------ */
  /* Settings persistence                                                */

  (function initPointerSensUi() {
    let pct = 100;
    const s = lsGet(PTR_SENS_KEY);
    if (s !== null) {
      const n = parseInt(s, 10);
      if (!isNaN(n)) pct = Math.max(25, Math.min(300, n));
    }
    ptrSensInput.value = String(pct);
    ptrSensVal.textContent = pct + "%";
    ptrSensInput.addEventListener("input", function () {
      ptrSensVal.textContent = parseInt(ptrSensInput.value, 10) + "%";
    });
    ptrSensInput.addEventListener("change", function () {
      lsSet(PTR_SENS_KEY, String(parseInt(ptrSensInput.value, 10)));
    });
  })();

  function pointerSensitivityMult() {
    return parseInt(ptrSensInput.value, 10) / 100;
  }

  /* ------------------------------------------------------------------ */
  /* Stats: client-side counters + device /stats polling                 */

  let showStats = lsGet(SHOW_STATS_KEY) === "true";
  let drawFrames = 0;
  let recvBytes = 0;
  let clientFps = 0;
  let clientMbps = 0;
  let deviceStats = null;
  let statsTimer = null;

  function renderStats() {
    if (!showStats) return;
    fpsDisplay.textContent = clientFps + " fps";
    let lines = ["net " + clientMbps.toFixed(1) + " Mbps"];
    if (deviceStats) {
      const d = deviceStats;
      lines.push(
        "cam " + d.cap_fps + " fps · enc " + d.enc_fps + " fps (" + (d.enc_us / 1000).toFixed(1) + " ms)" +
          (d.bs_us > 0 ? " · reorder " + (d.bs_us / 1000).toFixed(1) + " ms" : ""),
      );
      lines.push(
        "jpeg " + Math.round(d.jpeg_bytes / 1024) + " KB · q" + d.quality + " · " + d.pipeline +
          " · hdmi " + (d.hdmi_locked ? "locked" : "NO SIGNAL") +
          (d.recoveries ? " · rec " + d.recoveries : ""),
      );
    }
    statsOverlay.textContent = lines.join("\n");
  }

  async function pollDeviceStats() {
    try {
      const r = await fetch("/stats", { cache: "no-store" });
      if (r.ok) {
        deviceStats = await r.json();
        applyAtxAvailability(deviceStats);
        renderStats();
      }
    } catch (e) {
      /* device restarting */
    }
  }

  setInterval(function () {
    clientFps = drawFrames;
    clientMbps = (recvBytes * 8) / 1e6;
    drawFrames = 0;
    recvBytes = 0;
    renderStats();
  }, 1000);

  function applyShowStats(on) {
    showStats = on;
    fpsDisplay.classList.toggle("hidden", !on);
    statsOverlay.classList.toggle("hidden", !on);
    if (statsTimer) {
      clearInterval(statsTimer);
      statsTimer = null;
    }
    if (on) {
      pollDeviceStats();
      statsTimer = setInterval(pollDeviceStats, 2000);
    }
  }

  showStatsInput.checked = showStats;
  applyShowStats(showStats);
  showStatsInput.addEventListener("change", function () {
    lsSet(SHOW_STATS_KEY, String(showStatsInput.checked));
    applyShowStats(showStatsInput.checked);
  });

  /* ------------------------------------------------------------------ */
  /* MJPEG stream: incremental multipart parser + latest-frame decoding  */

  const canvasCtx = canvas.getContext("2d");
  let streamAbortController = null;
  let pendingJpeg = null; /* newest complete, not yet decoded frame */
  let painterWake = null;
  let painterRunning = false;

  /** Decode/draw loop decoupled from the network: if JPEGs arrive faster than
   *  the browser can decode, intermediate frames are skipped (lower latency
   *  than queueing them). */
  async function painterLoop() {
    painterRunning = true;
    while (painterRunning) {
      if (!pendingJpeg) {
        await new Promise(function (resolve) {
          painterWake = resolve;
        });
        painterWake = null;
        continue;
      }
      const jpeg = pendingJpeg;
      pendingJpeg = null;
      try {
        const bmp = await createImageBitmap(new Blob([jpeg], { type: "image/jpeg" }));
        canvasCtx.drawImage(bmp, 0, 0, W, H);
        bmp.close();
        drawFrames++;
      } catch (e) {
        /* corrupt frame, skip */
      }
    }
  }

  function submitFrame(jpegBytes) {
    pendingJpeg = jpegBytes;
    if (painterWake) painterWake();
  }

  const HEADER_END = new TextEncoder().encode("\r\n\r\n");
  const headerDecoder = new TextDecoder();

  function findBytesFrom(haystack, needle, from) {
    const nlen = needle.length;
    const limit = haystack.length - nlen;
    for (let i = from; i <= limit; i++) {
      let j = 0;
      while (j < nlen && haystack[i + j] === needle[j]) j++;
      if (j === nlen) return i;
    }
    return -1;
  }

  /**
   * Incremental multipart/x-mixed-replace parser. Headers (tiny) are
   * accumulated and scanned with a resumable offset; once Content-Length is
   * known the body bytes are written directly from each network chunk into
   * their final buffer - each JPEG byte is copied exactly once, no growing
   * concat buffer on the hot path.
   */
  function createMultipartParser(onFrame) {
    let head = new Uint8Array(0); /* partial header bytes between frames */
    let scanFrom = 0;
    let frame = null; /* body buffer being filled */
    let filled = 0;

    return function push(chunk) {
      while (chunk && chunk.length) {
        if (frame) {
          const take = Math.min(frame.length - filled, chunk.length);
          frame.set(chunk.subarray(0, take), filled);
          filled += take;
          chunk = chunk.subarray(take);
          if (filled === frame.length) {
            onFrame(frame);
            frame = null;
            filled = 0;
          }
          continue;
        }
        if (head.length === 0) {
          head = chunk;
        } else {
          const tmp = new Uint8Array(head.length + chunk.length);
          tmp.set(head);
          tmp.set(chunk, head.length);
          head = tmp;
        }
        chunk = null;
        const hEnd = findBytesFrom(head, HEADER_END, scanFrom);
        if (hEnd < 0) {
          scanFrom = Math.max(0, head.length - HEADER_END.length + 1);
          if (head.length > 16384) {
            /* A sane server never sends headers this long; keep the tail to resync. */
            head = head.slice(head.length - 4096);
            scanFrom = 0;
          }
          return;
        }
        const headerText = headerDecoder.decode(head.subarray(0, hEnd));
        const clMatch = headerText.match(/Content-Length:\s*(\d+)/i);
        const len = clMatch ? parseInt(clMatch[1], 10) : -1;
        const rest = head.subarray(hEnd + HEADER_END.length);
        head = new Uint8Array(0);
        scanFrom = 0;
        if (len >= 0 && len <= 8 * 1024 * 1024) {
          frame = new Uint8Array(len);
          filled = 0;
        }
        /* On a malformed part (no/absurd length) the remainder just re-enters
         * header scanning, which resyncs on the next boundary. */
        chunk = rest;
      }
    };
  }

  function sleep(ms) {
    return new Promise(function (resolve) {
      setTimeout(resolve, ms);
    });
  }

  async function startMjpegStream() {
    if (streamAbortController) streamAbortController.abort();
    streamAbortController = new AbortController();
    if (!painterRunning) painterLoop();

    for (;;) {
      try {
        const response = await fetch("/stream", {
          signal: streamAbortController.signal,
        });
        if (!response.ok) throw new Error("stream " + response.status);
        const reader = response.body.getReader();
        const push = createMultipartParser(submitFrame);
        for (;;) {
          const { done, value } = await reader.read();
          if (done) break;
          if (value) {
            recvBytes += value.length;
            push(value);
          }
        }
      } catch (e) {
        if (streamAbortController && streamAbortController.signal.aborted) return;
      }
      setStatus("Stream lost: reconnecting…");
      await sleep(1000);
    }
  }

  startMjpegStream();

  /* ------------------------------------------------------------------ */
  /* JPEG quality                                                        */

  function parseJpegQualityText(t) {
    const n = parseInt(String(t).trim(), 10);
    if (isNaN(n) || n < 1 || n > 100) return null;
    return n;
  }

  async function syncJpegQualityFromDevice() {
    try {
      const r = await fetch("/jpeg-quality", { cache: "no-store" });
      if (!r.ok) return;
      const n = parseJpegQualityText(await r.text());
      if (n !== null) jpegQInput.value = String(n);
    } catch (e) {
      /* device may still be starting */
    }
  }

  async function applyJpegQuality() {
    let q = parseInt(jpegQInput.value, 10);
    if (isNaN(q)) return;
    q = Math.max(1, Math.min(100, q));
    jpegQInput.value = String(q);
    try {
      const r = await fetch("/jpeg-quality?q=" + encodeURIComponent(q), {
        cache: "no-store",
      });
      if (r.ok) {
        const n = parseJpegQualityText(await r.text());
        if (n !== null) jpegQInput.value = String(n);
      }
    } catch (e) {
      /* ignore */
    }
  }

  btnJpegQ.addEventListener("click", applyJpegQuality);
  jpegQInput.addEventListener("keydown", function (ev) {
    if (ev.key === "Enter") {
      ev.preventDefault();
      applyJpegQuality();
    }
  });
  /* Staggered: don't add to the /stream + /ws connection burst at load. */
  let qualitySyncTimer = setTimeout(function () {
    qualitySyncTimer = null;
    syncJpegQualityFromDevice();
  }, 250);

  /* ------------------------------------------------------------------ */
  /* ATX host power buttons                                              */

  let atxInitDone = false;
  function applyAtxAvailability(d) {
    if (atxInitDone || !d) return;
    const power = !!d.atx_power;
    const reset = !!d.atx_reset;
    if (!power && !reset) return;
    atxInitDone = true;
    atxTitle.hidden = false;
    atxRow.hidden = false;
    btnAtxPower.hidden = !power;
    btnAtxForce.hidden = !power;
    btnAtxReset.hidden = !reset;
  }

  async function atxPress(op, confirmText) {
    if (confirmText && !window.confirm(confirmText)) return;
    try {
      const r = await fetch("/atx?op=" + op, { method: "POST" });
      if (r.ok) {
        setStatus("ATX: " + op + " sent");
      } else {
        setStatus("ATX " + op + " failed: " + r.status + " " + (await r.text()).trim());
      }
    } catch (e) {
      setStatus("ATX " + op + " failed (network)");
    }
  }

  btnAtxPower.addEventListener("click", function () {
    atxPress("power", "Tap the host power button? (boots the host or requests a soft shutdown)");
  });
  btnAtxReset.addEventListener("click", function () {
    atxPress("reset", "Reset the host? Unsaved data will be lost.");
  });
  btnAtxForce.addEventListener("click", function () {
    atxPress(
      "power_hold",
      "FORCE POWER OFF: holds the power button for 5 seconds and cuts the host hard. Unsaved data will be lost. Continue?",
    );
  });

  /*
   * Probe /stats so the power section appears even with the overlay off.
   * Delayed and retried: at page load /stream + /jpeg-quality already race a
   * freshly booted lwIP stack, and one failed probe must not hide the ATX
   * buttons forever.
   */
  let statsProbeTimer = null;
  function scheduleStatsProbe(attempt) {
    statsProbeTimer = setTimeout(
      async function () {
        statsProbeTimer = null;
        await pollDeviceStats();
        if (!deviceStats && attempt < 6) scheduleStatsProbe(attempt + 1);
      },
      attempt === 0 ? 800 : 3000,
    );
  }
  scheduleStatsProbe(0);

  /* ------------------------------------------------------------------ */
  /* WebSocket input channel                                             */

  const proto = location.protocol === "https:" ? "wss" : "ws";
  let ws = null;
  let reconnectTimer = null;

  function wsReady() {
    return !!ws && ws.readyState === 1;
  }

  const CODE_TO_HID = (function () {
    const m = {};
    for (let i = 0; i < 26; i++) {
      m["Key" + String.fromCharCode(65 + i)] = 0x04 + i;
    }
    const digits = [
      ["Digit1", 0x1e],
      ["Digit2", 0x1f],
      ["Digit3", 0x20],
      ["Digit4", 0x21],
      ["Digit5", 0x22],
      ["Digit6", 0x23],
      ["Digit7", 0x24],
      ["Digit8", 0x25],
      ["Digit9", 0x26],
      ["Digit0", 0x27],
    ];
    for (const [c, v] of digits) m[c] = v;
    const extra = [
      ["Enter", 0x28],
      ["Escape", 0x29],
      ["Backspace", 0x2a],
      ["Tab", 0x2b],
      ["Space", 0x2c],
      ["Minus", 0x2d],
      ["Equal", 0x2e],
      ["BracketLeft", 0x2f],
      ["BracketRight", 0x30],
      ["Backslash", 0x31],
      ["Semicolon", 0x33],
      ["Quote", 0x34],
      ["Backquote", 0x35],
      ["Comma", 0x36],
      ["Period", 0x37],
      ["Slash", 0x38],
      ["CapsLock", 0x39],
      ["F1", 0x3a],
      ["F2", 0x3b],
      ["F3", 0x3c],
      ["F4", 0x3d],
      ["F5", 0x3e],
      ["F6", 0x3f],
      ["F7", 0x40],
      ["F8", 0x41],
      ["F9", 0x42],
      ["F10", 0x43],
      ["F11", 0x44],
      ["F12", 0x45],
      ["PrintScreen", 0x46],
      ["ScrollLock", 0x47],
      ["Pause", 0x48],
      ["Insert", 0x49],
      ["Home", 0x4a],
      ["PageUp", 0x4b],
      ["Delete", 0x4c],
      ["End", 0x4d],
      ["PageDown", 0x4e],
      ["ArrowRight", 0x4f],
      ["ArrowLeft", 0x50],
      ["ArrowDown", 0x51],
      ["ArrowUp", 0x52],
      ["NumLock", 0x53],
      ["NumpadDivide", 0x54],
      ["NumpadMultiply", 0x55],
      ["NumpadSubtract", 0x56],
      ["NumpadAdd", 0x57],
      ["NumpadEnter", 0x58],
      ["Numpad1", 0x59],
      ["Numpad2", 0x5a],
      ["Numpad3", 0x5b],
      ["Numpad4", 0x5c],
      ["Numpad5", 0x5d],
      ["Numpad6", 0x5e],
      ["Numpad7", 0x5f],
      ["Numpad8", 0x60],
      ["Numpad9", 0x61],
      ["Numpad0", 0x62],
      ["NumpadDecimal", 0x63],
      ["ContextMenu", 0x65],
    ];
    for (const [c, v] of extra) m[c] = v;
    return m;
  })();

  const HELD = new Map();

  function hidModifierMask(ev) {
    let mod = 0;
    if (ev.ctrlKey) mod |= 0x01;
    if (ev.shiftKey) mod |= 0x02;
    if (ev.altKey) mod |= 0x04;
    if (ev.metaKey) mod |= 0x08;
    return mod;
  }

  function sendRawKeyboard(mod, keycodes) {
    if (!wsReady()) return;
    const k = keycodes.slice(0, 6);
    while (k.length < 6) k.push(0);
    const buf = new ArrayBuffer(8);
    const dv = new DataView(buf);
    dv.setUint8(0, 2);
    dv.setUint8(1, mod & 0xff);
    for (let i = 0; i < 6; i++) dv.setUint8(2 + i, k[i]);
    ws.send(buf);
  }

  function syncKeyboard(ev) {
    const keys = [];
    for (const k of HELD.keys()) {
      if (keys.length >= 6) break;
      keys.push(k);
    }
    sendRawKeyboard(hidModifierMask(ev), keys);
  }

  function releaseAllKeys() {
    HELD.clear();
    sendRawKeyboard(0, []);
  }

  const MOD_ONLY = new Set([
    "ControlLeft",
    "ControlRight",
    "ShiftLeft",
    "ShiftRight",
    "AltLeft",
    "AltRight",
    "MetaLeft",
    "MetaRight",
  ]);

  const HID_SHIFT = 0x02;

  function tapKey(mod, hid) {
    sendRawKeyboard(mod, hid ? [hid] : []);
    setTimeout(function () {
      sendRawKeyboard(0, []);
    }, 28);
  }

  /** US QWERTY: printable ASCII → { mod, hid } for paste (unknown chars skipped). */
  const PASTE_CHAR_TO_HID = (function () {
    const m = {};
    const SH = HID_SHIFT;
    function add(ch, mod, hid) {
      m[ch] = { mod: mod, hid: hid };
    }
    for (let i = 0; i < 26; i++) {
      add(String.fromCharCode(97 + i), 0, 0x04 + i);
      add(String.fromCharCode(65 + i), SH, 0x04 + i);
    }
    const dk = [0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27];
    const digs = "1234567890";
    for (let i = 0; i < 10; i++) add(digs[i], 0, dk[i]);
    const shifted = ")!@#$%^&*(";
    for (let i = 0; i < 10; i++) add(shifted[i], SH, dk[i]);
    add(" ", 0, 0x2c);
    add("\n", 0, 0x28);
    add("\r", 0, 0x28);
    add("\t", 0, 0x2b);
    add("-", 0, 0x2d);
    add("_", SH, 0x2d);
    add("=", 0, 0x2e);
    add("+", SH, 0x2e);
    add("[", 0, 0x2f);
    add("{", SH, 0x2f);
    add("]", 0, 0x30);
    add("}", SH, 0x30);
    add("\\", 0, 0x31);
    add("|", SH, 0x31);
    add(";", 0, 0x33);
    add(":", SH, 0x33);
    add("'", 0, 0x34);
    add('"', SH, 0x34);
    add("`", 0, 0x35);
    add("~", SH, 0x35);
    add(",", 0, 0x36);
    add("<", SH, 0x36);
    add(".", 0, 0x37);
    add(">", SH, 0x37);
    add("/", 0, 0x38);
    add("?", SH, 0x38);
    return m;
  })();

  async function typeStringAsHid(text) {
    for (let i = 0; i < text.length; i++) {
      const ch = text.charAt(i);
      if (ch === "\r" && text.charAt(i + 1) === "\n") {
        continue;
      }
      const row = PASTE_CHAR_TO_HID[ch];
      if (!row) continue;
      sendRawKeyboard(row.mod, [row.hid]);
      await sleep(28);
      sendRawKeyboard(0, []);
      await sleep(18);
    }
  }

  function updateHidToolButtons() {
    const ok = wsReady();
    btnSendEsc.disabled = !ok;
    btnCad.disabled = !ok;
    btnPasteClip.disabled = !ok;
  }

  btnSendEsc.addEventListener("click", function () {
    if (!wsReady()) return;
    tapKey(0, 0x29);
  });

  btnCad.addEventListener("click", function () {
    if (!wsReady()) return;
    /* Ctrl (0x01) + Alt (0x04) + Delete (0x4c) */
    tapKey(0x05, 0x4c);
  });

  btnPasteClip.addEventListener("click", function () {
    if (!wsReady()) return;
    if (!navigator.clipboard || !navigator.clipboard.readText) {
      setStatus("Clipboard API unavailable (use HTTPS or localhost)");
      return;
    }
    btnPasteClip.disabled = true;
    navigator.clipboard
      .readText()
      .then(function (text) {
        return typeStringAsHid(text);
      })
      .then(function () {
        updateHidToolButtons();
        updateModeUi();
      })
      .catch(function () {
        setStatus("Clipboard read denied or failed (grant permission / use HTTPS)");
        updateHidToolButtons();
      });
  });

  /* ------------------------------------------------------------------ */
  /* Keyboard capture                                                    */

  function onKeyDown(ev) {
    if (!kbdCaptured()) return;
    if (MOD_ONLY.has(ev.code)) {
      ev.preventDefault();
      syncKeyboard(ev);
      return;
    }
    /* Esc in relative mode: the browser exits pointer lock regardless, but the
     * key-down is still forwarded so the host receives a real Esc tap; the
     * pointerlockchange handler sends all-keys-up right after. */
    const h = CODE_TO_HID[ev.code];
    if (h === undefined) return;
    ev.preventDefault();
    HELD.set(h, true);
    syncKeyboard(ev);
  }

  function onKeyUp(ev) {
    if (!kbdCaptured()) return;
    if (MOD_ONLY.has(ev.code)) {
      ev.preventDefault();
      syncKeyboard(ev);
      return;
    }
    const h = CODE_TO_HID[ev.code];
    if (h === undefined) return;
    ev.preventDefault();
    HELD.delete(h);
    syncKeyboard(ev);
  }

  window.addEventListener("keydown", onKeyDown, true);
  window.addEventListener("keyup", onKeyUp, true);
  window.addEventListener("blur", function () {
    if (HELD.size === 0) return;
    releaseAllKeys();
  });

  canvas.addEventListener("focus", updateModeUi);
  canvas.addEventListener("blur", function () {
    if (mode === "abs" && HELD.size > 0) releaseAllKeys();
    updateModeUi();
  });

  /* ------------------------------------------------------------------ */
  /* Mouse handling                                                      */

  /** Last pointer position in frame pixels (rel-mode virtual position and
   *  button release fallback). */
  let lastX = 0,
    lastY = 0;

  function scaleMovementToFrame() {
    const r = canvas.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) {
      return { sx: 1, sy: 1 };
    }
    return { sx: (W - 1) / r.width, sy: (H - 1) / r.height };
  }

  /** Update lastX/lastY from pointer-lock mickeys (matches scaled deltas sent over WS). */
  function applyRelativeFromEvent(ev) {
    const mx = ev.movementX || 0;
    const my = ev.movementY || 0;
    const { sx, sy } = scaleMovementToFrame();
    const sens = pointerSensitivityMult();
    const rdx = Math.round(mx * sx * sens);
    const rdy = Math.round(my * sy * sens);
    let x = lastX + rdx;
    let y = lastY + rdy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= W) x = W - 1;
    if (y >= H) y = H - 1;
    lastX = x;
    lastY = y;
    return { rdx, rdy };
  }

  let wasPointerLocked = false;

  function onPointerLockChange() {
    const locked = pointerLockActive();
    if (wasPointerLocked && !locked) {
      releaseAllKeys();
    }
    wasPointerLocked = locked;
    canvas.classList.toggle("pointer-locked", locked);
    updateModeUi();
  }

  document.addEventListener("pointerlockchange", onPointerLockChange);
  document.addEventListener("pointerlockerror", function () {
    updateModeUi();
    setStatus("Pointer lock failed (try HTTPS, or another browser)");
  });

  let pending = null;
  let raf = 0;

  function flushMouse() {
    raf = 0;
    if (!pending || !wsReady()) return;
    const p = pending;
    pending = null;
    const buf = new ArrayBuffer(8);
    const dv = new DataView(buf);
    dv.setUint8(0, 1);
    dv.setUint8(1, p.buttons);
    if (p.mode === "rel") {
      const clamp16 = function (v) {
        return Math.max(-32768, Math.min(32767, v));
      };
      dv.setInt16(2, clamp16(p.dx), true);
      dv.setInt16(4, clamp16(p.dy), true);
      dv.setUint8(7, 1);
    } else {
      dv.setUint16(2, p.x, true);
      dv.setUint16(4, p.y, true);
      dv.setUint8(7, 0);
    }
    dv.setInt8(6, p.wheel);
    ws.send(buf);
  }

  function flushPendingIfOtherMode(m) {
    if (!pending || pending.mode === m) return;
    if (raf) {
      cancelAnimationFrame(raf);
      raf = 0;
    }
    flushMouse();
  }

  /**
   * Coalesce moves to one WebSocket frame per animation frame (reduces load).
   * Always flush immediately for wheel or any button transition so clicks are not merged away.
   */
  function queueMouseAbs(buttons, x, y, wheel, forceImmediate) {
    flushPendingIfOtherMode("abs");
    const prev = pending;
    const btnChanged = prev !== null && prev.mode === "abs" && prev.buttons !== buttons;
    let w = wheel;
    if (prev && prev.mode === "abs") {
      w = clampWheel(prev.wheel + wheel);
    }
    pending = { mode: "abs", buttons: buttons, x: x, y: y, wheel: w };
    if (forceImmediate || wheel !== 0 || btnChanged) {
      if (raf) {
        cancelAnimationFrame(raf);
        raf = 0;
      }
      flushMouse();
      return;
    }
    if (!raf) raf = requestAnimationFrame(flushMouse);
  }

  function queueMouseRel(buttons, dx, dy, wheel, forceImmediate) {
    flushPendingIfOtherMode("rel");
    const prev = pending;
    const btnChanged = prev !== null && prev.mode === "rel" && prev.buttons !== buttons;
    if (!pending || pending.mode !== "rel") {
      pending = {
        mode: "rel",
        buttons: buttons,
        dx: dx,
        dy: dy,
        wheel: wheel,
      };
    } else {
      pending.buttons = buttons;
      pending.dx += dx;
      pending.dy += dy;
      pending.wheel = clampWheel(pending.wheel + wheel);
    }
    if (forceImmediate || wheel !== 0 || btnChanged) {
      if (raf) {
        cancelAnimationFrame(raf);
        raf = 0;
      }
      flushMouse();
      return;
    }
    if (!raf) raf = requestAnimationFrame(flushMouse);
  }

  function clampWheel(v) {
    return Math.max(-127, Math.min(127, v));
  }

  /** Wheel event → HID wheel step (shared by both pointer modes). */
  function wheelStep(ev) {
    return clampWheel(Math.round(-ev.deltaY / 16));
  }

  function mapXY(ev) {
    const r = canvas.getBoundingClientRect();
    let x = ev.clientX - r.left;
    let y = ev.clientY - r.top;
    if (r.width <= 0 || r.height <= 0) return { x: 0, y: 0 };
    x = Math.round((x / r.width) * (W - 1));
    y = Math.round((y / r.height) * (H - 1));
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= W) x = W - 1;
    if (y >= H) y = H - 1;
    return { x, y };
  }

  function mouseButtons(ev) {
    let b = 0;
    if (ev.buttons & 1) b |= 1;
    if (ev.buttons & 2) b |= 2;
    if (ev.buttons & 4) b |= 4;
    /* mousedown: some engines lag updating `buttons`; use `button` for the activating click. */
    if (
      (ev.type === "pointerdown" || ev.type === "mousedown") &&
      b === 0 &&
      typeof ev.button === "number"
    ) {
      if (ev.button === 0) b |= 1;
      else if (ev.button === 1) b |= 4;
      else if (ev.button === 2) b |= 2;
    }
    return b;
  }

  function isMousePointer(ev) {
    return !ev.pointerType || ev.pointerType === "mouse" || ev.pointerType === "pen";
  }

  /* --- Tablet (absolute) mode: no capture needed, hover moves the host cursor --- */

  canvas.addEventListener("pointermove", function (ev) {
    if (mode !== "abs" || pointerLockActive() || !isMousePointer(ev)) return;
    const { x, y } = mapXY(ev);
    lastX = x;
    lastY = y;
    queueMouseAbs(mouseButtons(ev), x, y, 0, false);
  });

  canvas.addEventListener("pointerdown", function (ev) {
    if (!isMousePointer(ev)) return;
    if (mode === "abs") {
      canvas.focus(); /* keyboard capture; preventDefault below would block it */
      ev.preventDefault();
      try {
        canvas.setPointerCapture(ev.pointerId); /* keep drags outside the canvas */
      } catch (e) {
        /* pointer already gone */
      }
      const { x, y } = mapXY(ev);
      lastX = x;
      lastY = y;
      queueMouseAbs(mouseButtons(ev), x, y, 0, true);
      return;
    }
    /* Relative mode: first click acquires the pointer lock. */
    const { x, y } = mapXY(ev);
    lastX = x;
    lastY = y;
    if (
      ev.button === 0 &&
      !pointerLockActive() &&
      typeof canvas.requestPointerLock === "function"
    ) {
      ev.preventDefault();
      const req = canvas.requestPointerLock();
      if (req && typeof req.catch === "function") {
        req.catch(function () {
          /* insecure context or user gesture policy */
        });
      }
    }
  });

  canvas.addEventListener("pointerup", function (ev) {
    if (mode !== "abs" || pointerLockActive() || !isMousePointer(ev)) return;
    ev.preventDefault();
    const { x, y } = mapXY(ev);
    lastX = x;
    lastY = y;
    queueMouseAbs(mouseButtons(ev), x, y, 0, true);
  });

  canvas.addEventListener("wheel", function (ev) {
    if (mode !== "abs" || pointerLockActive()) return;
    ev.preventDefault();
    const w = wheelStep(ev);
    if (w === 0) return;
    const { x, y } = mapXY(ev);
    queueMouseAbs(mouseButtons(ev), x, y, w, true);
  }, { passive: false });

  canvas.addEventListener("contextmenu", function (ev) {
    ev.preventDefault();
  });

  /* --- Relative mode: movement and buttons are delivered on the document while locked --- */

  document.addEventListener("mousemove", function (ev) {
    if (!pointerLockActive()) return;
    const { rdx, rdy } = applyRelativeFromEvent(ev);
    queueMouseRel(mouseButtons(ev), rdx, rdy, 0, false);
  });
  document.addEventListener("mousedown", function (ev) {
    if (!pointerLockActive()) return;
    queueMouseRel(mouseButtons(ev), 0, 0, 0, true);
  });
  document.addEventListener("mouseup", function (ev) {
    if (!pointerLockActive()) return;
    queueMouseRel(mouseButtons(ev), 0, 0, 0, true);
  });
  document.addEventListener(
    "wheel",
    function (ev) {
      if (!pointerLockActive()) return;
      ev.preventDefault();
      queueMouseRel(mouseButtons(ev), 0, 0, wheelStep(ev), true);
    },
    { passive: false },
  );

  /* ------------------------------------------------------------------ */
  /* Fullscreen                                                          */

  btnFullscreen.addEventListener("click", function () {
    if (document.fullscreenElement) {
      document.exitFullscreen();
    } else if (kvmWrap.requestFullscreen) {
      kvmWrap.requestFullscreen();
    }
  });

  /* ------------------------------------------------------------------ */
  /* WebSocket lifecycle                                                 */

  function connectWs() {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    ws = new WebSocket(proto + "://" + location.host + "/ws");
    ws.binaryType = "arraybuffer";
    ws.onopen = function () {
      updateModeUi();
      updateHidToolButtons();
    };
    ws.onclose = function () {
      if (pointerLockActive()) {
        document.exitPointerLock();
      }
      setStatus("Input disconnected: retrying…");
      updateHidToolButtons();
      reconnectTimer = setTimeout(connectWs, 2000);
    };
    ws.onerror = function () {
      ws.close();
    };
  }

  let initialWsTimer = null;
  if (import.meta.hot) {
    import.meta.hot.dispose(function () {
      painterRunning = false;
      if (painterWake) painterWake();
      if (streamAbortController) {
        streamAbortController.abort();
        streamAbortController = null;
      }
      if (initialWsTimer) {
        clearTimeout(initialWsTimer);
        initialWsTimer = null;
      }
      if (statsProbeTimer) {
        clearTimeout(statsProbeTimer);
        statsProbeTimer = null;
      }
      if (qualitySyncTimer) {
        clearTimeout(qualitySyncTimer);
        qualitySyncTimer = null;
      }
      if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
      }
      if (statsTimer) {
        clearInterval(statsTimer);
        statsTimer = null;
      }
      if (ws) {
        ws.onclose = null;
        ws.onerror = null;
        ws.close();
        ws = null;
      }
    });
  }

  updateModeUi();

  /* Let /stream canvas fetch and /jpeg-quality complete first; tight lwIP + httpd was seeing ECONNRESET when all three raced. */
  initialWsTimer = setTimeout(function () {
    initialWsTimer = null;
    connectWs();
  }, 400);
})();
