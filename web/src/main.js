import "./style.css";
import "@xterm/xterm/css/xterm.css";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";

(function () {
  /* Frame size follows the device's video mode (updated from /stats). */
  let frameW = 1920,
    frameH = 1080;
  /* HID absolute axes are resolution-independent: 0..32767 on the wire. */
  const ABS_MAX = 32767;

  /* ---------------- DOM ---------------- */
  const $ = (id) => document.getElementById(id);
  const canvas = $("kvm");
  const stage = $("stage");
  const stageWrap = $("stage-wrap");
  const statusPill = $("status");
  const stagehint = $("stagehint");
  const nosignal = $("nosignal");
  const nsReason = $("ns-reason");
  const nsHint = $("ns-hint");
  const teleFps = $("tele-fps");
  const teleMbps = $("tele-mbps");
  const btnModeAbs = $("mode-abs");
  const btnModeRel = $("mode-rel");
  const viewSeg = $("view-seg");
  const ptrModeSeg = $("ptr-mode-seg");
  const btnViewKvm = $("view-kvm");
  const btnViewTerm = $("view-term");
  const termWrap = $("term-wrap");
  const termEl = $("terminal");
  const termDot = $("term-dot");
  const termState = $("term-state");
  const btnFullscreen = $("btn-fullscreen");
  const btnHud = $("btn-hud");
  const hud = $("hud");
  const hudPtr = $("hud-ptr");
  const hudHeld = $("hud-held");
  const hudKeys = $("hud-keys");
  const btnAudio = $("btn-audio");
  const btnDrawer = $("btn-drawer");
  const btnDrawerClose = $("btn-drawer-close");
  const drawer = $("drawer");
  const scrim = $("scrim");
  const btnSendEsc = $("btn-send-esc");
  const btnCad = $("btn-cad");
  const btnPasteClip = $("btn-paste-clip");
  const secPower = $("sec-power");
  const btnAtxPower = $("btn-atx-power");
  const btnAtxReset = $("btn-atx-reset");
  const btnAtxForce = $("btn-atx-force");
  const jpegQ = $("jpeg-q");
  const qVal = $("q-val");
  const maxFps = $("max-fps");
  const fpsVal = $("fps-val");
  const resVal = $("res-val");
  const btnRes720 = $("res-720");
  const btnRes1080 = $("res-1080");
  const ptrSens = $("ptr-sens");
  const sensVal = $("sens-val");
  const btnMedia = $("btn-media");
  const mediaPop = $("media-pop");
  const mediaLed = $("media-led");
  const mediaStatus = $("media-status");
  const mediaFile = $("media-file");
  const mediaWritable = $("media-writable");
  const btnMediaMount = $("btn-media-mount");
  const btnMediaEject = $("btn-media-eject");
  const mediaProgWrap = $("media-prog-wrap");
  const mediaProg = $("media-prog");
  const mediaProgPct = $("media-prog-pct");
  const diagEl = $("diag");
  const atxNote = $("atx-note");
  const cfSsid = $("cf-ssid");
  const cfPass = $("cf-pass");
  const cfPassState = $("cf-pass-state");
  const cfWgEp = $("cf-wg-ep");
  const cfWgPort = $("cf-wg-port");
  const cfWgPriv = $("cf-wg-priv");
  const cfWgPrivState = $("cf-wg-priv-state");
  const cfWgPub = $("cf-wg-pub");
  const cfWgPsk = $("cf-wg-psk");
  const cfWgPskState = $("cf-wg-psk-state");
  const cfWgIp = $("cf-wg-ip");
  const cfAtxPwr = $("cf-atx-pwr");
  const cfAtxRst = $("cf-atx-rst");
  const cfAtxLvl = $("cf-atx-lvl");
  const cfTurnUrl = $("cf-turn-url");
  const cfTurnSecret = $("cf-turn-secret");
  const cfTurnSecretState = $("cf-turn-secret-state");
  const btnCfgSave = $("btn-cfg-save");
  const drFoot = $("dr-foot");

  const MODE_KEY = "p4kvm_pointer_mode";
  const PTR_SENS_KEY = "p4kvm_pointer_sensitivity_pct";

  function lsGet(k) {
    try { return localStorage.getItem(k); } catch (e) { return null; }
  }
  function lsSet(k, v) {
    try { localStorage.setItem(k, v); } catch (e) { /* private mode */ }
  }

  /* ---------------- status / hint ---------------- */

  function setStatus(text, tone) {
    statusPill.textContent = text;
    statusPill.dataset.tone = tone || "off";
  }
  function setHint(text) {
    stagehint.textContent = text || "";
  }

  /* ---------------- pointer mode ---------------- */

  let mode = lsGet(MODE_KEY) === "rel" ? "rel" : "abs";

  function pointerLockActive() {
    return document.pointerLockElement === canvas;
  }
  function kbdCaptured() {
    return mode === "rel" ? pointerLockActive() : document.activeElement === canvas;
  }

  function refreshInputUi() {
    btnModeAbs.classList.toggle("active", mode === "abs");
    btnModeRel.classList.toggle("active", mode === "rel");
    const captured = kbdCaptured();
    stage.classList.toggle("captured", captured);
    canvas.classList.toggle("pointer-locked", pointerLockActive());
    if (!hidReady()) {
      return; /* connection state owns the pill while the link is down */
    }
    if (captured) {
      setStatus(mode === "rel" ? "LOCKED" : "CAPTURED", "cap");
      setHint(mode === "rel" ? "Esc releases the pointer" : "keyboard captured — click outside the video to release");
    } else {
      setStatus("READY", "ok");
      setHint(mode === "rel"
        ? "click the video to lock pointer + keyboard"
        : "move over the video to point · click it to capture the keyboard");
    }
  }

  function setMode(next) {
    if (next === mode) return;
    releaseAllKeys();
    if (pointerLockActive()) document.exitPointerLock();
    if (document.activeElement === canvas) canvas.blur();
    mode = next;
    lsSet(MODE_KEY, mode);
    refreshInputUi();
  }

  btnModeAbs.addEventListener("click", () => setMode("abs"));
  btnModeRel.addEventListener("click", () => setMode("rel"));

  /* ---------------- drawer ---------------- */

  function drawerOpen(open) {
    drawer.classList.toggle("open", open);
    drawer.setAttribute("aria-hidden", String(!open));
    btnDrawer.setAttribute("aria-expanded", String(open));
    scrim.hidden = !open;
  }
  btnDrawer.addEventListener("click", () => drawerOpen(!drawer.classList.contains("open")));
  btnDrawerClose.addEventListener("click", () => drawerOpen(false));
  scrim.addEventListener("click", () => drawerOpen(false));

  /* ---------------- client telemetry ---------------- */

  let drawFrames = 0;
  let recvBytes = 0;
  let lastFrameAt = 0;

  setInterval(function () {
    teleFps.textContent = String(drawFrames);
    teleMbps.textContent = ((recvBytes * 8) / 1e6).toFixed(1);
    drawFrames = 0;
    recvBytes = 0;
  }, 1000);

  /* ---------------- MJPEG stream engine ---------------- */

  const canvasCtx = canvas.getContext("2d");
  let streamAbortController = null;
  let streamUp = false;
  let pendingJpeg = null;
  let painterWake = null;
  let painterRunning = false;

  /* Decode loop decoupled from the network: stale frames are replaced, not
   * queued, so a slow decoder costs frames instead of latency. */
  async function painterLoop() {
    painterRunning = true;
    while (painterRunning) {
      if (!pendingJpeg) {
        await new Promise((resolve) => { painterWake = resolve; });
        painterWake = null;
        continue;
      }
      const jpeg = pendingJpeg;
      pendingJpeg = null;
      try {
        const bmp = await createImageBitmap(new Blob([jpeg], { type: "image/jpeg" }));
        canvasCtx.drawImage(bmp, 0, 0, frameW, frameH);
        bmp.close();
        drawFrames++;
        lastFrameAt = performance.now();
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
   * their final buffer - each JPEG byte is copied exactly once.
   */
  function createMultipartParser(onFrame) {
    let head = new Uint8Array(0);
    let scanFrom = 0;
    let frame = null;
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
        chunk = rest; /* malformed part: remainder resyncs via header scan */
      }
    };
  }

  function sleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
  }

  async function startMjpegStream() {
    if (streamAbortController) streamAbortController.abort();
    streamAbortController = new AbortController();
    if (!painterRunning) painterLoop();

    for (;;) {
      try {
        const response = await fetch("/stream", { signal: streamAbortController.signal });
        if (!response.ok) throw new Error("stream " + response.status);
        streamUp = true;
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
      streamUp = false;
      await sleep(1000);
    }
  }

  /* ---------------- WebRTC (hardware H.264 video + HID data channel) ----------------
   *
   * Progressive enhancement over MJPEG: the device answers a browser offer via
   * POST /webrtc/offer (automatic signaling, no external server). When the H.264
   * track goes live the canvas MJPEG is stopped and the <video> shows through the
   * (now transparent) canvas, which stays the input surface. Keyboard/mouse then
   * ride the "hid" SCTP data channel; if the peer connection ever fails, HID
   * falls back to /ws and MJPEG resumes - so the KVM keeps working regardless.
   */
  const video = $("kvm-video");
  let pc = null;
  let hidChannel = null;
  let webrtcActive = false;
  const webrtcDisabled = /[?&]nowebrtc=1/.test(location.search) || typeof RTCPeerConnection === "undefined";
  const WEBRTC_LOG = /[?&]webrtclog=1/.test(location.search);

  /* IPv4/IPv6 literal test — only literals are valid in a host ICE candidate. */
  function isIpLiteral(h) {
    return /^\d{1,3}(\.\d{1,3}){3}$/.test(h) || (h.indexOf(":") >= 0 && /^[0-9a-fA-F:]+$/.test(h));
  }

  /* Same-LAN rescue. This esp_peer build does not enumerate local interfaces, so
   * it never advertises a host candidate for the device's LAN IP — it only offers
   * a STUN-reflexive (public) candidate. When browser and device sit behind the
   * same NAT, reaching that public address needs router hairpinning, which most
   * home routers refuse, so ICE stalls. But the browser reached the device
   * directly to load this page, so location.hostname IS a routable path to it.
   * Synthesize a host candidate at that IP for every UDP port the device
   * advertised (a port-preserving NAT maps the local port unchanged, the common
   * case) so the browser probes the device directly instead of via hairpin.
   * Harmless if wrong — ICE just discards a candidate that never answers. */
  function synthLanCandidates(deviceCands) {
    const host = location.hostname;
    if (!isIpLiteral(host)) return []; /* accessed by name: can't form a host candidate */
    const ports = new Set();
    for (const c of deviceCands) {
      const m = /(?:udp|UDP)\s+\d+\s+([0-9a-fA-F.:]+)\s+(\d+)\s+typ\s+(host|srflx)/.exec(c);
      if (m) ports.add(m[2]);
    }
    const out = [];
    let i = 0;
    for (const p of ports) {
      /* Foundation/priority are cosmetic here; a high host-typ priority makes ICE
       * try this pair early. sdpMLineIndex is set by the caller. */
      out.push("candidate:lan" + i + " 1 udp " + (2130706431 - i) + " " + host + " " + p + " typ host generation 0");
      i++;
    }
    return out;
  }

  function hidChannelOpen() {
    return !!hidChannel && hidChannel.readyState === "open";
  }

  /* Input is available over either transport: the WebRTC data channel or /ws. */
  function hidReady() {
    return wsReady() || hidChannelOpen();
  }

  /* HID transport: data channel when open, else the WebSocket. Same wire format. */
  function hidSend(buf) {
    if (hidChannelOpen()) {
      try { hidChannel.send(buf); return; } catch (e) { /* fall through to ws */ }
    }
    if (wsReady()) ws.send(buf);
  }

  function setTransportDiag() {
    dset("transport", webrtcActive ? "WebRTC · H.264" : "MJPEG", webrtcActive ? "ok" : null);
  }

  function showWebrtcVideo(on) {
    webrtcActive = on;
    if (on) {
      video.hidden = false;
      stage.classList.add("webrtc");
      if (streamAbortController) { streamAbortController.abort(); streamAbortController = null; }
      streamUp = false;
      try { canvasCtx.clearRect(0, 0, canvas.width, canvas.height); } catch (e) {}
    } else {
      video.hidden = true;
      stage.classList.remove("webrtc");
      try { video.srcObject = null; } catch (e) {}
      if (!streamAbortController) startMjpegStream();
    }
    setTransportDiag();
  }

  let webrtcAttempts = 0;
  /* One attempt per page. Each negotiation opens ICE/DTLS sockets on the device,
   * whose lwIP pool is small; on a network where WebRTC can't pair, retrying
   * hammered that pool until accept() failed with ENFILE and wedged the HTTP
   * server. So: try once, and if it fails record it for the browser session so
   * reloads don't restart the storm. The user stays on MJPEG, which is reliable.
   * ?webrtcforce=1 clears the sticky flag to retry after a network/TURN change. */
  const WEBRTC_MAX_ATTEMPTS = 1;
  const WEBRTC_OFF_KEY = "p4kvm-webrtc-off";
  const webrtcForced = /[?&]webrtcforce=1/.test(location.search);
  function webrtcGaveUp() {
    try { return !webrtcForced && sessionStorage.getItem(WEBRTC_OFF_KEY) === "1"; } catch (e) { return false; }
  }
  function markWebrtcGaveUp() {
    try { sessionStorage.setItem(WEBRTC_OFF_KEY, "1"); } catch (e) {}
  }

  function teardownWebrtc(retry) {
    if (hidChannel) { try { hidChannel.close(); } catch (e) {} hidChannel = null; }
    if (pc) { try { pc.close(); } catch (e) {} pc = null; }
    if (webrtcActive) showWebrtcVideo(false);
    /* Give up for this session rather than renegotiating - a failed pairing means
     * the network can't carry it (no TURN, mDNS-hidden host candidates, etc.),
     * and retrying only churns device sockets. MJPEG stays as the transport. */
    if (retry && webrtcAttempts >= WEBRTC_MAX_ATTEMPTS) {
      markWebrtcGaveUp();
    }
  }

  function iceGatheringComplete(peer, timeoutMs) {
    if (peer.iceGatheringState === "complete") return Promise.resolve();
    return new Promise((resolve) => {
      let done = false;
      const finish = () => {
        if (done) return;
        done = true;
        peer.removeEventListener("icegatheringstatechange", check);
        resolve();
      };
      const check = () => { if (peer.iceGatheringState === "complete") finish(); };
      peer.addEventListener("icegatheringstatechange", check);
      setTimeout(finish, timeoutMs); /* non-trickle: proceed with candidates gathered so far */
    });
  }

  async function fetchIceServers() {
    /* The device serves STUN plus, when configured, a TURN relay with freshly
     * derived short-lived credentials (GET /webrtc/ice). Must be fetched before
     * the RTCPeerConnection so gathering includes relay candidates. Falls back to
     * public STUN alone if the device has no TURN configured or the call fails. */
    const fallback = [{ urls: "stun:stun.l.google.com:19302" }];
    try {
      const r = await fetch("/webrtc/ice", { cache: "no-store" });
      if (!r.ok) return fallback;
      const j = await r.json();
      if (Array.isArray(j.iceServers) && j.iceServers.length) {
        if (WEBRTC_LOG) console.log("[webrtc] iceServers:", JSON.stringify(j.iceServers.map((s) => s.urls)));
        return j.iceServers;
      }
    } catch (e) { /* offline / no endpoint */ }
    return fallback;
  }

  async function startWebrtc() {
    if (webrtcDisabled || pc || webrtcGaveUp()) return;
    webrtcAttempts++;
    try {
      const iceServers = await fetchIceServers();
      /* An ICE server is required (esp_peer only gathers with one). A TURN relay,
       * if present, carries the media when direct/host pairing can't cross the
       * network (same-NAT hairpin, station isolation, or an off-LAN viewer). */
      pc = new RTCPeerConnection({ iceServers });
      hidChannel = pc.createDataChannel("hid", { ordered: true });
      pc.addTransceiver("video", { direction: "recvonly" });
      pc.ontrack = function (ev) {
        video.srcObject = ev.streams && ev.streams[0] ? ev.streams[0] : new MediaStream([ev.track]);
        video.play().catch(function () {});
        showWebrtcVideo(true);
      };
      pc.onconnectionstatechange = function () {
        const s = pc && pc.connectionState;
        if (WEBRTC_LOG) console.log("[webrtc] connectionState:", s);
        if (s === "failed" || s === "disconnected" || s === "closed") teardownWebrtc(true);
      };
      if (WEBRTC_LOG) {
        pc.oniceconnectionstatechange = function () {
          console.log("[webrtc] iceConnectionState:", pc && pc.iceConnectionState);
        };
        pc.onicecandidateerror = function (e) {
          console.log("[webrtc] icecandidateerror:", e.errorCode, e.errorText, e.url);
        };
      }
      await pc.setLocalDescription(await pc.createOffer());
      await iceGatheringComplete(pc, 2500);
      const resp = await fetch("/webrtc/offer", {
        method: "POST",
        headers: { "Content-Type": "application/sdp" },
        body: pc.localDescription.sdp,
      });
      if (!resp.ok) throw new Error("signal " + resp.status);
      const ans = await resp.json();
      await pc.setRemoteDescription({ type: "answer", sdp: ans.sdp });
      if (Array.isArray(ans.candidates)) {
        for (const c of ans.candidates) {
          if (WEBRTC_LOG) console.log("[webrtc] device candidate:", c);
          try { await pc.addIceCandidate({ candidate: c, sdpMLineIndex: 0 }); } catch (e) {}
        }
        for (const c of synthLanCandidates(ans.candidates)) {
          if (WEBRTC_LOG) console.log("[webrtc] synth LAN candidate:", c);
          try { await pc.addIceCandidate({ candidate: c, sdpMLineIndex: 0 }); } catch (e) {}
        }
      }
      /* Give ICE/DTLS time to come up (STUN round-trip + pairing); otherwise fall back. */
      setTimeout(function () { if (pc && !webrtcActive) teardownWebrtc(true); }, 15000);
    } catch (e) {
      teardownWebrtc(true);
    }
  }

  /* ---------------- device stats / diagnostics ---------------- */

  const DIAG_ROWS = [
    ["stream", "STREAM"],
    ["transport", "TRANSPORT"],
    ["input", "INPUT LINK"],
    ["usb", "USB HID"],
    ["hdmi", "HDMI SOURCE"],
    ["pipeline", "PIPELINE"],
    ["capfps", "CAPTURE"],
    ["encfps", "ENCODE"],
    ["txfps", "STREAM RATE"],
    ["jpeg", "JPEG SIZE"],
    ["net", "LINK RATE"],
    ["quality", "QUALITY"],
    ["clients", "VIEWERS"],
    ["vpn", "WIREGUARD"],
    ["audio", "AUDIO"],
    ["serial", "SERIAL"],
    ["recover", "RECOVERIES"],
    ["encerr", "ENC ERRORS"],
    ["mem", "HEAP / PSRAM"],
    ["uptime", "UPTIME"],
    ["net-if", "ADDRESSES"],
  ];
  const diagDd = {};
  for (const [key, label] of DIAG_ROWS) {
    const row = document.createElement("div");
    row.className = "dr";
    const dt = document.createElement("dt");
    dt.textContent = label;
    const dd = document.createElement("dd");
    dd.id = "diag-" + key;
    row.appendChild(dt);
    row.appendChild(dd);
    diagEl.appendChild(row);
    diagDd[key] = dd;
  }

  function dset(key, text, tone) {
    const dd = diagDd[key];
    if (!dd) return;
    dd.textContent = "";
    if (tone) {
      const dot = document.createElement("span");
      dot.className = "dot";
      dd.appendChild(dot);
      dd.dataset.tone = tone;
    } else {
      delete dd.dataset.tone;
    }
    dd.appendChild(document.createTextNode(text));
  }

  function fmtUptime(s) {
    if (s == null) return "--";
    const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600),
      m = Math.floor((s % 3600) / 60);
    if (d) return d + "d " + h + "h";
    if (h) return h + "h " + m + "m";
    return m + "m " + (s % 60) + "s";
  }
  function fmtKb(b) {
    return b >= 1048576 ? (b / 1048576).toFixed(1) + " MB" : Math.round(b / 1024) + " KB";
  }

  let deviceStats = null;
  let statsFresh = false;
  let atxInitDone = false;
  let restarting = false;

  function applyVideoMode(d) {
    if (!d || !d.width || !d.height) return;
    if (canvas.width !== d.width || canvas.height !== d.height) {
      canvas.width = d.width;
      canvas.height = d.height;
    }
    frameW = d.width;
    frameH = d.height;
    resVal.textContent = d.mode || "--";
    btnRes720.classList.toggle("active", d.mode === "720p60");
    btnRes1080.classList.toggle("active", d.mode === "1080p30");
  }

  function applyAtxAvailability(d) {
    if (atxInitDone || !d) return;
    const power = !!d.atx_power;
    const reset = !!d.atx_reset;
    if (!power && !reset) return;
    atxInitDone = true;
    btnAtxPower.hidden = !power;
    btnAtxForce.hidden = !power;
    btnAtxReset.hidden = !reset;
    atxNote.hidden = true;
  }

  /** SYS_STATUS bits (TC358743): 0x01 DDC5V, 0x02 TMDS, 0x80 SYNC. */
  function hdmiDecode(d) {
    if (!d) return { text: "unknown", tone: "warn" };
    if (String(d.pipeline).indexOf("testpat") >= 0) return { text: "test pattern", tone: "ok" };
    if (d.hdmi_locked) return { text: "locked", tone: "ok" };
    const st = d.sys_status | 0;
    if (!(st & 0x01)) return { text: "no source (DDC 5V absent)", tone: "err" };
    if (!(st & 0x02)) return { text: "source present, no TMDS", tone: "warn" };
    if (!(st & 0x80)) return { text: "TMDS up, no sync", tone: "warn" };
    return { text: "acquiring (0x" + st.toString(16) + ")", tone: "warn" };
  }

  function renderDiagnostics() {
    dset("stream", streamUp ? "connected" : "reconnecting", streamUp ? "ok" : "err");
    dset("input", hidReady() ? "connected" : "reconnecting", hidReady() ? "ok" : "err");
    if (webrtcActive) {
      const fps = deviceStats && deviceStats.h264_fps ? " · " + deviceStats.h264_fps + " fps" : "";
      dset("transport", "WebRTC · H.264" + fps, "ok");
    }
    dset("net", teleMbps.textContent + " Mb/s · draw " + teleFps.textContent + " fps");
    const d = deviceStats;
    if (!d || !statsFresh) {
      dset("hdmi", "device unreachable", "err");
      return;
    }
    const hd = hdmiDecode(d);
    dset("hdmi", hd.text, hd.tone);
    dset("usb", d.usb_hid ? "mounted" : "not mounted", d.usb_hid ? "ok" : "warn");
    dset("pipeline", String(d.pipeline));
    dset("capfps", d.cap_fps + " fps", d.cap_fps > 0 ? "ok" : "warn");
    let enc = d.enc_fps + " fps · " + (d.enc_us / 1000).toFixed(1) + " ms";
    if (d.bs_us > 0) enc += " · reorder " + (d.bs_us / 1000).toFixed(1) + " ms";
    dset("encfps", enc, d.enc_fps > 0 ? "ok" : "warn");
    if (typeof d.tx_fps === "number") {
      const capTxt = d.max_fps ? " (cap " + d.max_fps + ")" : "";
      dset("txfps", d.tx_fps + " fps" + capTxt, "ok");
    }
    dset("jpeg", d.jpeg_bytes ? fmtKb(d.jpeg_bytes) + " / frame" : "--");
    dset("quality", "q" + d.quality);
    const wg = d.wg || "off";
    dset("vpn", wg, wg === "up" ? "ok" : wg === "off" ? undefined : "warn");
    const au = d.audio || "off";
    dset("audio", audioOn ? "playing" : au, au === "streaming" || audioOn ? "ok" : undefined);
    btnAudio.hidden = au === "off";
    const ser = d.serial || "off";
    viewSeg.hidden = ser === "off";
    dset("serial", ser, ser === "open" ? "ok" : ser === "off" ? undefined : "warn");
    /* When the terminal is open, mirror the device's port state on the dot
     * (the WS being up only tells us the browser link, not the target's DTR). */
    if (view === "term" && serialWs && serialWs.readyState === 1) {
      setTermDot(ser === "open" ? "open" : "closed");
    }
    dset("clients", String(d.clients));
    dset("recover", String(d.recoveries), d.recoveries > 0 ? "warn" : undefined);
    dset("encerr", String(d.enc_errors), d.enc_errors > 0 ? "err" : undefined);
    dset("mem", fmtKb(d.heap_free) + " / " + fmtKb(d.psram_free));
    dset("uptime", fmtUptime(d.uptime_s));
    const addrs = [d.ip_eth, d.ip_wifi].filter(Boolean).join(" · ");
    dset("net-if", addrs || "--");
    drFoot.textContent =
      (d.hostname ? d.hostname + ".local" : "p4kvm") + " · v" + (d.version || "?") + " · " + d.pipeline;
  }

  async function pollDeviceStats() {
    try {
      const r = await fetch("/stats", { cache: "no-store" });
      if (r.ok) {
        deviceStats = await r.json();
        statsFresh = true;
        restarting = false;
        applyAtxAvailability(deviceStats);
        applyVideoMode(deviceStats);
        if (!qDragging && deviceStats.quality !== parseInt(jpegQ.value, 10)) {
          jpegQ.value = String(deviceStats.quality);
          qVal.textContent = "q" + deviceStats.quality;
        }
        if (!fpsDragging && typeof deviceStats.max_fps === "number" &&
            deviceStats.max_fps >= 1 && deviceStats.max_fps !== parseInt(maxFps.value, 10)) {
          maxFps.value = String(deviceStats.max_fps);
          fpsVal.textContent = fpsLabel(deviceStats.max_fps);
        }
        /* Reveal + refresh the media button once, when the device first answers.
         * Afterwards it refreshes only on popover open / mount / eject. */
        if (btnMedia.hidden) refreshMediaStatus();
        return;
      }
    } catch (e) {
      /* device restarting / unreachable */
    }
    statsFresh = false;
  }

  /* ---------------- no-signal presentation ---------------- */

  function noSignalReason() {
    if (restarting) return { r: "RESTARTING", h: "applying the new video mode — back in about 10 seconds" };
    if (!streamUp) return { r: "STREAM OFFLINE", h: "reconnecting to the device…" };
    const d = statsFresh ? deviceStats : null;
    if (!d) return { r: "DEVICE UNREACHABLE", h: "the video stream is open but /stats does not answer" };
    const hd = hdmiDecode(d);
    if (String(d.pipeline).indexOf("testpat") >= 0 || d.hdmi_locked) {
      if (d.cap_fps > 0 && d.enc_fps === 0)
        return { r: "CAPTURING · ENCODER STALLED", h: "frames arrive from the source but JPEG encoding produces nothing — check the serial log" };
      return { r: "WAITING FOR FRAMES", h: "source is up; no frames published yet" };
    }
    if (!(d.sys_status & 0x01))
      return { r: "NO SOURCE · DDC +5V ABSENT", h: "no HDMI input module or no cable/host attached — connect the source, or build with the test-pattern option to run without it" };
    return { r: hd.text.toUpperCase(), h: "source detected but not locked — the recovery ladder retries automatically (" + (d.recoveries || 0) + " so far)" };
  }

  setInterval(function () {
    const stale = performance.now() - lastFrameAt > 2500;
    /* A static desktop legitimately stops producing frames: content-dedup only
     * transmits on change, so "no frame for 2.5 s" is the normal idle state, not
     * a signal loss. Keep the last painted frame on screen as long as we have one
     * and both the stream and the source are healthy — only fall back to the
     * NO SIGNAL overlay when the frame is stale AND something is actually wrong
     * (never received a frame, stream offline, device unreachable, or the source
     * dropped / lost lock, which /stats reflects within ~1 s). */
    const everHadFrame = lastFrameAt > 0;
    const d = statsFresh ? deviceStats : null;
    const sourceLive = !!d && (String(d.pipeline).indexOf("testpat") >= 0 || !!d.hdmi_locked);
    const staticScreen = everHadFrame && streamUp && sourceLive;
    if (stale && !staticScreen) {
      const { r, h } = noSignalReason();
      nsReason.textContent = r;
      nsHint.textContent = h;
      nosignal.hidden = false;
    } else {
      nosignal.hidden = true;
    }
    renderDiagnostics();
  }, 1000);

  /* ---------------- quality slider ---------------- */

  let qDragging = false;
  let qDebounce = null;
  qVal.textContent = "q" + jpegQ.value;
  jpegQ.addEventListener("pointerdown", () => { qDragging = true; });
  jpegQ.addEventListener("pointerup", () => { qDragging = false; });
  jpegQ.addEventListener("input", function () {
    qVal.textContent = "q" + jpegQ.value;
    if (qDebounce) clearTimeout(qDebounce);
    qDebounce = setTimeout(async function () {
      qDebounce = null;
      try {
        await fetch("/jpeg-quality?q=" + encodeURIComponent(jpegQ.value), { cache: "no-store" });
      } catch (e) { /* retried implicitly by next adjustment */ }
    }, 300);
  });

  async function syncJpegQualityFromDevice() {
    try {
      const r = await fetch("/jpeg-quality", { cache: "no-store" });
      if (!r.ok) return;
      const n = parseInt((await r.text()).trim(), 10);
      if (!isNaN(n) && n >= 1 && n <= 100) {
        jpegQ.value = String(n);
        qVal.textContent = "q" + n;
      }
    } catch (e) { /* device may still be starting */ }
  }

  /* ---------------- max framerate ---------------- */

  let fpsDragging = false;
  let fpsDebounce = null;
  function fpsLabel(v) { return v + " fps"; }
  fpsVal.textContent = fpsLabel(maxFps.value);
  maxFps.addEventListener("pointerdown", () => { fpsDragging = true; });
  maxFps.addEventListener("pointerup", () => { fpsDragging = false; });
  maxFps.addEventListener("input", function () {
    fpsVal.textContent = fpsLabel(maxFps.value);
    if (fpsDebounce) clearTimeout(fpsDebounce);
    fpsDebounce = setTimeout(async function () {
      fpsDebounce = null;
      try {
        await fetch("/stream-fps?fps=" + encodeURIComponent(maxFps.value), { cache: "no-store" });
      } catch (e) { /* retried implicitly by next adjustment */ }
    }, 300);
  });

  async function syncMaxFpsFromDevice() {
    try {
      const r = await fetch("/stream-fps", { cache: "no-store" });
      if (!r.ok) return;
      const n = parseInt((await r.text()).trim(), 10);
      if (!isNaN(n) && n >= 1 && n <= 60) {
        maxFps.value = String(n);
        fpsVal.textContent = fpsLabel(n);
      }
    } catch (e) { /* device may still be starting */ }
  }

  /* ---------------- setup form (/config) ---------------- */

  async function loadConfigForm() {
    try {
      const r = await fetch("/config", { cache: "no-store" });
      if (!r.ok) return;
      const c = await r.json();
      cfSsid.value = c.wifi_ssid || "";
      cfPassState.textContent = c.wifi_pass_set ? "(set)" : "(not set)";
      cfWgEp.value = c.wg_endpoint || "";
      cfWgPort.value = String(c.wg_port || 51820);
      cfWgPrivState.textContent = c.wg_priv_set ? "(set)" : "(not set)";
      cfWgPub.value = c.wg_peer_pubkey || "";
      cfWgPskState.textContent = c.wg_psk_set ? "(set)" : "(optional, not set)";
      cfWgIp.value = c.wg_local_ip || "";
      cfAtxPwr.value = String(c.atx_power_gpio);
      cfAtxRst.value = String(c.atx_reset_gpio);
      cfAtxLvl.checked = !!c.atx_active_high;
      cfTurnUrl.value = c.turn_url || "";
      cfTurnSecretState.textContent = c.turn_secret_set ? "(set)" : "(not set)";
    } catch (e) {
      /* device restarting */
    }
  }

  btnCfgSave.addEventListener("click", async function () {
    if (!window.confirm("Save settings and restart the device (about 10 s)?")) return;
    const body = {
      wifi_ssid: cfSsid.value.trim(),
      wg_endpoint: cfWgEp.value.trim(),
      wg_port: parseInt(cfWgPort.value, 10) || 51820,
      wg_peer_pubkey: cfWgPub.value.trim(),
      wg_local_ip: cfWgIp.value.trim(),
      atx_power_gpio: parseInt(cfAtxPwr.value, 10),
      atx_reset_gpio: parseInt(cfAtxRst.value, 10),
      atx_active_high: cfAtxLvl.checked,
      turn_url: cfTurnUrl.value.trim(),
    };
    /* Secrets: only send when the user typed something (empty = keep). */
    if (cfPass.value) body.wifi_pass = cfPass.value;
    if (cfWgPriv.value) body.wg_private_key = cfWgPriv.value.trim();
    if (cfWgPsk.value) body.wg_psk = cfWgPsk.value.trim();
    if (cfTurnSecret.value) body.turn_secret = cfTurnSecret.value.trim();
    try {
      const r = await fetch("/config?reboot=1", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (r.ok) {
        restarting = true;
        statsFresh = false;
        setStatus("RESTARTING", "warn");
        setHint("settings saved, device restarting…");
        drawerOpen(false);
      } else {
        setHint("save failed: " + r.status);
      }
    } catch (e) {
      setHint("save failed (network)");
    }
  });

  /* ---------------- resolution ---------------- */

  async function switchVideoMode(mode) {
    if (deviceStats && deviceStats.mode === mode) return;
    if (!window.confirm("Switch to " + mode + "? The device restarts (about 10 s) and the host re-detects the display.")) return;
    try {
      const r = await fetch("/video-mode?mode=" + encodeURIComponent(mode), { method: "POST" });
      if (r.ok) {
        restarting = true;
        statsFresh = false;
        setStatus("RESTARTING", "warn");
        setHint("device restarting into " + mode + "…");
      } else {
        setHint("mode switch failed: " + r.status);
      }
    } catch (e) {
      setHint("mode switch failed (network)");
    }
  }
  btnRes720.addEventListener("click", () => switchVideoMode("720p60"));
  btnRes1080.addEventListener("click", () => switchVideoMode("1080p30"));

  /* ---------------- sensitivity ---------------- */

  (function initSens() {
    let pct = 100;
    const s = lsGet(PTR_SENS_KEY);
    if (s !== null) {
      const n = parseInt(s, 10);
      if (!isNaN(n)) pct = Math.max(25, Math.min(300, n));
    }
    ptrSens.value = String(pct);
    sensVal.textContent = pct + "%";
    ptrSens.addEventListener("input", function () {
      sensVal.textContent = parseInt(ptrSens.value, 10) + "%";
    });
    ptrSens.addEventListener("change", function () {
      lsSet(PTR_SENS_KEY, String(parseInt(ptrSens.value, 10)));
    });
  })();

  function pointerSensitivityMult() {
    return parseInt(ptrSens.value, 10) / 100;
  }

  /* ---------------- ATX ---------------- */

  async function atxPress(op, confirmText) {
    if (confirmText && !window.confirm(confirmText)) return;
    try {
      const r = await fetch("/atx?op=" + op, { method: "POST" });
      setHint(r.ok ? "ATX " + op + " sent" : "ATX " + op + " failed: " + r.status);
    } catch (e) {
      setHint("ATX " + op + " failed (network)");
    }
  }
  btnAtxPower.addEventListener("click", () =>
    atxPress("power", "Tap the host power button? (boots the host or requests a soft shutdown)"));
  btnAtxReset.addEventListener("click", () =>
    atxPress("reset", "Reset the host? Unsaved data will be lost."));
  btnAtxForce.addEventListener("click", () =>
    atxPress("power_hold",
      "FORCE POWER OFF: holds the power button for 5 seconds and cuts the host hard. Unsaved data will be lost. Continue?"));

  /* ---------------- virtual media ---------------- */

  function fmtBytes(n) {
    if (n >= 1024 * 1024) return (n / (1024 * 1024)).toFixed(2) + " MiB";
    if (n >= 1024) return (n / 1024).toFixed(0) + " KiB";
    return n + " B";
  }

  const FLOPPY_BYTES = 1474560; /* 1.44 MB blank floppy = "no image mounted" */

  function applyMediaState(s) {
    if (!s || !s.available) {
      mediaStatus.textContent = "not ready";
      btnMedia.classList.remove("on");
      return;
    }
    btnMedia.hidden = false;
    const mounted = s.present && s.size_bytes !== FLOPPY_BYTES;
    btnMedia.classList.toggle("on", mounted);
    btnMedia.title = mounted
      ? "Virtual media: " + fmtBytes(s.size_bytes) + " mounted"
      : "Virtual media (USB drive)";
    if (!s.present) {
      mediaStatus.textContent = "ejected";
    } else if (mounted) {
      mediaStatus.textContent = fmtBytes(s.size_bytes) + (s.writable ? " · read/write" : " · read-only");
    } else {
      mediaStatus.textContent = "empty floppy";
    }
  }

  async function refreshMediaStatus() {
    try {
      const r = await fetch("/media/status", { cache: "no-store" });
      if (!r.ok) { applyMediaState(null); return; }
      applyMediaState(await r.json());
    } catch (e) {
      applyMediaState(null);
    }
  }

  function mediaPopOpen(open) {
    mediaPop.hidden = !open;
    btnMedia.setAttribute("aria-expanded", String(open));
    if (open) refreshMediaStatus();
  }
  btnMedia.addEventListener("click", (ev) => {
    ev.stopPropagation();
    mediaPopOpen(mediaPop.hidden);
  });
  /* Dismiss on outside click / Esc, but not when interacting inside the popover. */
  mediaPop.addEventListener("click", (ev) => ev.stopPropagation());
  document.addEventListener("click", () => { if (!mediaPop.hidden) mediaPopOpen(false); });
  document.addEventListener("keydown", (ev) => {
    if (ev.key === "Escape" && !mediaPop.hidden) mediaPopOpen(false);
  });

  mediaFile.addEventListener("change", function () {
    btnMediaMount.disabled = !(mediaFile.files && mediaFile.files.length);
  });

  /* Stream the raw file body with XHR so we get upload progress (fetch has no
   * request-progress event). The device sizes the ramdisk from Content-Length. */
  function mountImage(file, writable) {
    return new Promise(function (resolve, reject) {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", "/media/image?writable=" + (writable ? "1" : "0"));
      xhr.setRequestHeader("Content-Type", "application/octet-stream");
      xhr.upload.onprogress = function (ev) {
        if (!ev.lengthComputable) return;
        const pct = Math.round((ev.loaded / ev.total) * 100);
        mediaProg.value = pct;
        mediaProgPct.textContent = pct + "%";
      };
      xhr.onload = function () {
        (xhr.status >= 200 && xhr.status < 300) ? resolve() : reject(new Error("HTTP " + xhr.status));
      };
      xhr.onerror = function () { reject(new Error("network")); };
      xhr.send(file);
    });
  }

  btnMediaMount.addEventListener("click", async function () {
    const file = mediaFile.files && mediaFile.files[0];
    if (!file) return;
    if (file.size > 12 * 1024 * 1024) {
      setHint("image too large: " + fmtBytes(file.size) + " (max 12 MiB PSRAM)");
      return;
    }
    btnMediaMount.disabled = true;
    btnMediaEject.disabled = true;
    mediaProgWrap.hidden = false;
    mediaProg.value = 0;
    mediaProgPct.textContent = "0%";
    setHint("uploading " + file.name + " (" + fmtBytes(file.size) + ")…");
    try {
      await mountImage(file, mediaWritable.checked);
      setHint("mounted " + file.name + " — the target now sees it as a USB drive");
    } catch (e) {
      setHint("mount failed: " + e.message);
    } finally {
      mediaProgWrap.hidden = true;
      btnMediaEject.disabled = false;
      btnMediaMount.disabled = !(mediaFile.files && mediaFile.files.length);
      refreshMediaStatus();
    }
  });

  btnMediaEject.addEventListener("click", async function () {
    btnMediaEject.disabled = true;
    try {
      const r = await fetch("/media/eject", { method: "POST" });
      setHint(r.ok ? "ejected — reverted to a blank floppy" : "eject failed: " + r.status);
    } catch (e) {
      setHint("eject failed (network)");
    } finally {
      btnMediaEject.disabled = false;
      refreshMediaStatus();
    }
  });

  /* ---------------- serial console (xterm.js over /serial) ---------------- */

  let view = "kvm"; /* "kvm" | "term" */
  let term = null;
  let termFit = null;
  let serialWs = null;
  let serialReconnect = null;
  const termEncoder = new TextEncoder();

  function ensureTerminal() {
    if (term) return;
    term = new Terminal({
      convertEol: false,
      cursorBlink: true,
      fontFamily: 'ui-monospace, "SF Mono", Menlo, Consolas, monospace',
      fontSize: 13,
      scrollback: 5000,
      theme: {
        background: "#05070a",
        foreground: "#d7dde8",
        cursor: "#ffb454",
        selectionBackground: "rgba(255,180,84,0.3)",
      },
    });
    termFit = new FitAddon();
    term.loadAddon(termFit);
    term.open(termEl);
    fitTerm();
    /* Browser keystrokes -> target. Send as UTF-8 bytes over the binary WS. */
    term.onData((data) => {
      if (serialWs && serialWs.readyState === 1) {
        serialWs.send(termEncoder.encode(data));
      }
    });
  }

  function fitTerm() {
    if (termFit && view === "term") {
      try { termFit.fit(); } catch (e) { /* container not laid out yet */ }
    }
  }

  function setTermDot(state) {
    termDot.classList.toggle("open", state === "open");
    termDot.classList.toggle("closed", state === "closed");
    termState.textContent =
      state === "open" ? "serial console — port open"
      : state === "closed" ? "serial console — waiting for target to open the port"
      : "serial console — disconnected";
  }

  function serialConnect() {
    if (serialWs && (serialWs.readyState === 0 || serialWs.readyState === 1)) return;
    const ws = new WebSocket(proto + "://" + location.host + "/serial");
    ws.binaryType = "arraybuffer";
    serialWs = ws;
    ws.onopen = () => setTermDot("closed");
    ws.onmessage = (ev) => {
      if (!term) return;
      if (ev.data instanceof ArrayBuffer) {
        term.write(new Uint8Array(ev.data));
      } else if (typeof ev.data === "string") {
        term.write(ev.data);
      }
    };
    ws.onclose = () => {
      setTermDot("off");
      if (view === "term") {
        clearTimeout(serialReconnect);
        serialReconnect = setTimeout(serialConnect, 1200);
      }
    };
    ws.onerror = () => { try { ws.close(); } catch (e) {} };
  }

  function serialDisconnect() {
    clearTimeout(serialReconnect);
    if (serialWs) {
      try { serialWs.close(); } catch (e) {}
      serialWs = null;
    }
  }

  function setView(next) {
    if (next === view) return;
    view = next;
    const term_active = view === "term";
    btnViewKvm.classList.toggle("active", !term_active);
    btnViewTerm.classList.toggle("active", term_active);
    termWrap.hidden = !term_active;
    /* Pointer-mode control is meaningless in the terminal; hide it there. */
    ptrModeSeg.hidden = term_active;
    if (term_active) {
      /* Release any KVM capture so keystrokes go to the terminal, not HID. */
      releaseAllKeys();
      if (pointerLockActive()) document.exitPointerLock();
      if (document.activeElement === canvas) canvas.blur();
      ensureTerminal();
      serialConnect();
      requestAnimationFrame(() => { fitTerm(); term && term.focus(); });
    } else {
      serialDisconnect();
      setTermDot("off");
    }
  }

  btnViewKvm.addEventListener("click", () => setView("kvm"));
  btnViewTerm.addEventListener("click", () => setView("term"));
  window.addEventListener("resize", fitTerm);

  /* ---------------- WebSocket input ---------------- */

  const proto = location.protocol === "https:" ? "wss" : "ws";
  let ws = null;
  let reconnectTimer = null;

  function wsReady() {
    return !!ws && ws.readyState === 1;
  }

  const CODE_TO_HID = (function () {
    const m = {};
    for (let i = 0; i < 26; i++) m["Key" + String.fromCharCode(65 + i)] = 0x04 + i;
    const table = [
      ["Digit1", 0x1e], ["Digit2", 0x1f], ["Digit3", 0x20], ["Digit4", 0x21],
      ["Digit5", 0x22], ["Digit6", 0x23], ["Digit7", 0x24], ["Digit8", 0x25],
      ["Digit9", 0x26], ["Digit0", 0x27],
      ["Enter", 0x28], ["Escape", 0x29], ["Backspace", 0x2a], ["Tab", 0x2b],
      ["Space", 0x2c], ["Minus", 0x2d], ["Equal", 0x2e], ["BracketLeft", 0x2f],
      ["BracketRight", 0x30], ["Backslash", 0x31], ["Semicolon", 0x33],
      ["Quote", 0x34], ["Backquote", 0x35], ["Comma", 0x36], ["Period", 0x37],
      ["Slash", 0x38], ["CapsLock", 0x39],
      ["F1", 0x3a], ["F2", 0x3b], ["F3", 0x3c], ["F4", 0x3d], ["F5", 0x3e],
      ["F6", 0x3f], ["F7", 0x40], ["F8", 0x41], ["F9", 0x42], ["F10", 0x43],
      ["F11", 0x44], ["F12", 0x45],
      ["PrintScreen", 0x46], ["ScrollLock", 0x47], ["Pause", 0x48],
      ["Insert", 0x49], ["Home", 0x4a], ["PageUp", 0x4b], ["Delete", 0x4c],
      ["End", 0x4d], ["PageDown", 0x4e],
      ["ArrowRight", 0x4f], ["ArrowLeft", 0x50], ["ArrowDown", 0x51], ["ArrowUp", 0x52],
      ["NumLock", 0x53], ["NumpadDivide", 0x54], ["NumpadMultiply", 0x55],
      ["NumpadSubtract", 0x56], ["NumpadAdd", 0x57], ["NumpadEnter", 0x58],
      ["Numpad1", 0x59], ["Numpad2", 0x5a], ["Numpad3", 0x5b], ["Numpad4", 0x5c],
      ["Numpad5", 0x5d], ["Numpad6", 0x5e], ["Numpad7", 0x5f], ["Numpad8", 0x60],
      ["Numpad9", 0x61], ["Numpad0", 0x62], ["NumpadDecimal", 0x63],
      ["ContextMenu", 0x65],
    ];
    for (const [c, v] of table) m[c] = v;
    return m;
  })();

  const HELD = new Map();

  /* ---------------- debug HUD ---------------- */

  /* HID usage → short label, derived from CODE_TO_HID so it always matches. */
  const HID_TO_LABEL = (function () {
    const m = {};
    for (const code in CODE_TO_HID) {
      m[CODE_TO_HID[code]] = code
        .replace(/^Key/, "")
        .replace(/^Digit/, "")
        .replace(/^Numpad/, "KP")
        .replace(/^Arrow/, "");
    }
    return m;
  })();
  function hidLabel(h) {
    return HID_TO_LABEL[h] || "0x" + h.toString(16);
  }

  const HUD_KEY = "p4kvm_hud";
  let hudOn = lsGet(HUD_KEY) !== "0"; /* default on: it's a debugging aid */
  let lastMod = 0;
  const recentKeys = [];

  function modLabels(mod) {
    const p = [];
    if (mod & 0x01) p.push("Ctrl");
    if (mod & 0x04) p.push("Alt");
    if (mod & 0x02) p.push("Shift");
    if (mod & 0x08) p.push("Meta");
    return p;
  }
  function ptrBtns(b) {
    return (b & 1 ? "L" : "·") + (b & 4 ? "M" : "·") + (b & 2 ? "R" : "·");
  }
  function updatePtrHud(p) {
    if (!hudOn) return;
    if (p.mode === "rel") {
      const sx = (p.dx >= 0 ? "+" : "") + p.dx, sy = (p.dy >= 0 ? "+" : "") + p.dy;
      hudPtr.textContent =
        "REL  d " + sx + "," + sy + "  btn " + ptrBtns(p.buttons) + (p.wheel ? "  whl " + p.wheel : "");
    } else {
      const px = Math.round((p.x / ABS_MAX) * (frameW - 1));
      const py = Math.round((p.y / ABS_MAX) * (frameH - 1));
      hudPtr.textContent =
        "ABS  " + p.x + "," + p.y + "  px " + px + "," + py +
        "  btn " + ptrBtns(p.buttons) + (p.wheel ? "  whl " + p.wheel : "");
    }
  }
  function renderKbdHud() {
    if (!hudOn) return;
    const held = modLabels(lastMod);
    for (const h of HELD.keys()) held.push(hidLabel(h));
    hudHeld.textContent = held.length ? held.join(" ") : "--";
    hudKeys.textContent = recentKeys.length ? recentKeys.slice(-8).join(" ") : "--";
  }
  function logKey(mod, base) {
    recentKeys.push(modLabels(mod).concat(base).join("+"));
    while (recentKeys.length > 12) recentKeys.shift();
    renderKbdHud();
  }
  function setHud(on) {
    hudOn = on;
    hud.classList.toggle("on", on);
    btnHud.classList.toggle("active", on);
    btnHud.setAttribute("aria-pressed", String(on));
    lsSet(HUD_KEY, on ? "1" : "0");
    if (on) renderKbdHud();
  }
  btnHud.addEventListener("click", () => setHud(!hudOn));

  function hidModifierMask(ev) {
    let mod = 0;
    if (ev.ctrlKey) mod |= 0x01;
    if (ev.shiftKey) mod |= 0x02;
    if (ev.altKey) mod |= 0x04;
    if (ev.metaKey) mod |= 0x08;
    return mod;
  }

  function sendRawKeyboard(mod, keycodes) {
    if (!wsReady() && !hidChannelOpen()) return;
    const k = keycodes.slice(0, 6);
    while (k.length < 6) k.push(0);
    const buf = new ArrayBuffer(8);
    const dv = new DataView(buf);
    dv.setUint8(0, 2);
    dv.setUint8(1, mod & 0xff);
    for (let i = 0; i < 6; i++) dv.setUint8(2 + i, k[i]);
    hidSend(buf);
  }

  function syncKeyboard(ev) {
    const keys = [];
    for (const k of HELD.keys()) {
      if (keys.length >= 6) break;
      keys.push(k);
    }
    lastMod = hidModifierMask(ev);
    sendRawKeyboard(lastMod, keys);
    renderKbdHud();
  }

  function releaseAllKeys() {
    HELD.clear();
    lastMod = 0;
    sendRawKeyboard(0, []);
    renderKbdHud();
  }

  const MOD_ONLY = new Set([
    "ControlLeft", "ControlRight", "ShiftLeft", "ShiftRight",
    "AltLeft", "AltRight", "MetaLeft", "MetaRight",
  ]);

  const HID_SHIFT = 0x02;

  function tapKey(mod, hid) {
    sendRawKeyboard(mod, hid ? [hid] : []);
    setTimeout(() => sendRawKeyboard(0, []), 28);
  }

  /** US QWERTY: printable ASCII → { mod, hid } for paste (unknown chars skipped). */
  const PASTE_CHAR_TO_HID = (function () {
    const m = {};
    const SH = HID_SHIFT;
    const add = (ch, mod, hid) => { m[ch] = { mod, hid }; };
    for (let i = 0; i < 26; i++) {
      add(String.fromCharCode(97 + i), 0, 0x04 + i);
      add(String.fromCharCode(65 + i), SH, 0x04 + i);
    }
    const dk = [0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27];
    const digs = "1234567890";
    const shifted = ")!@#$%^&*(";
    for (let i = 0; i < 10; i++) {
      add(digs[i], 0, dk[i]);
      add(shifted[i], SH, dk[i]);
    }
    add(" ", 0, 0x2c); add("\n", 0, 0x28); add("\r", 0, 0x28); add("\t", 0, 0x2b);
    add("-", 0, 0x2d); add("_", SH, 0x2d); add("=", 0, 0x2e); add("+", SH, 0x2e);
    add("[", 0, 0x2f); add("{", SH, 0x2f); add("]", 0, 0x30); add("}", SH, 0x30);
    add("\\", 0, 0x31); add("|", SH, 0x31); add(";", 0, 0x33); add(":", SH, 0x33);
    add("'", 0, 0x34); add('"', SH, 0x34); add("`", 0, 0x35); add("~", SH, 0x35);
    add(",", 0, 0x36); add("<", SH, 0x36); add(".", 0, 0x37); add(">", SH, 0x37);
    add("/", 0, 0x38); add("?", SH, 0x38);
    return m;
  })();

  async function typeStringAsHid(text) {
    for (let i = 0; i < text.length; i++) {
      const ch = text.charAt(i);
      if (ch === "\r" && text.charAt(i + 1) === "\n") continue;
      const row = PASTE_CHAR_TO_HID[ch];
      if (!row) continue;
      sendRawKeyboard(row.mod, [row.hid]);
      await sleep(28);
      sendRawKeyboard(0, []);
      await sleep(18);
    }
  }

  function updateHidToolButtons() {
    const ok = hidReady();
    btnSendEsc.disabled = !ok;
    btnCad.disabled = !ok;
    btnPasteClip.disabled = !ok;
  }

  btnSendEsc.addEventListener("click", function () {
    if (!hidReady()) return;
    tapKey(0, 0x29);
    logKey(0, "Escape");
  });
  btnCad.addEventListener("click", function () {
    /* Ctrl (0x01) + Alt (0x04) + Delete (0x4c) */
    if (!hidReady()) return;
    tapKey(0x05, 0x4c);
    logKey(0x05, "Delete");
  });
  btnPasteClip.addEventListener("click", function () {
    if (!hidReady()) return;
    if (!navigator.clipboard || !navigator.clipboard.readText) {
      setHint("clipboard API unavailable (use HTTPS or localhost)");
      return;
    }
    btnPasteClip.disabled = true;
    navigator.clipboard
      .readText()
      .then((text) => { logKey(0, "paste(" + text.length + ")"); return typeStringAsHid(text); })
      .catch(() => setHint("clipboard read denied (grant permission / use HTTPS)"))
      .finally(() => {
        updateHidToolButtons();
        refreshInputUi();
      });
  });

  /* ---------------- keyboard capture ---------------- */

  function onKeyDown(ev) {
    if (!kbdCaptured()) return;
    if (MOD_ONLY.has(ev.code)) {
      ev.preventDefault();
      syncKeyboard(ev);
      return;
    }
    /* Esc in relative mode: the browser exits pointer lock regardless, but the
     * key-down still reaches the host; pointerlockchange sends all-keys-up. */
    const h = CODE_TO_HID[ev.code];
    if (h === undefined) return;
    ev.preventDefault();
    if (!ev.repeat && !HELD.has(h)) logKey(hidModifierMask(ev), hidLabel(h));
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
    if (HELD.size > 0) releaseAllKeys();
  });

  canvas.addEventListener("focus", refreshInputUi);
  canvas.addEventListener("blur", function () {
    if (mode === "abs" && HELD.size > 0) releaseAllKeys();
    refreshInputUi();
  });

  /* ---------------- mouse ---------------- */

  function scaleMovementToFrame() {
    const r = canvas.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) return { sx: 1, sy: 1 };
    return { sx: (frameW - 1) / r.width, sy: (frameH - 1) / r.height };
  }

  function applyRelativeFromEvent(ev) {
    const { sx, sy } = scaleMovementToFrame();
    const sens = pointerSensitivityMult();
    const rdx = Math.round((ev.movementX || 0) * sx * sens);
    const rdy = Math.round((ev.movementY || 0) * sy * sens);
    return { rdx, rdy };
  }

  let wasPointerLocked = false;
  document.addEventListener("pointerlockchange", function () {
    const locked = pointerLockActive();
    if (wasPointerLocked && !locked) releaseAllKeys();
    wasPointerLocked = locked;
    refreshInputUi();
  });
  document.addEventListener("pointerlockerror", function () {
    refreshInputUi();
    setHint("pointer lock failed (try HTTPS, or another browser)");
  });

  let pending = null;
  let raf = 0;

  function clampWheel(v) {
    return Math.max(-127, Math.min(127, v));
  }
  function wheelStep(ev) {
    return clampWheel(Math.round(-ev.deltaY / 16));
  }

  function flushMouse() {
    raf = 0;
    if (!pending) return;
    updatePtrHud(pending); /* reflect the report even while the link is down */
    if (!hidReady()) return;
    const p = pending;
    pending = null;
    const buf = new ArrayBuffer(8);
    const dv = new DataView(buf);
    dv.setUint8(0, 1);
    dv.setUint8(1, p.buttons);
    if (p.mode === "rel") {
      const c16 = (v) => Math.max(-32768, Math.min(32767, v));
      dv.setInt16(2, c16(p.dx), true);
      dv.setInt16(4, c16(p.dy), true);
      dv.setUint8(7, 1);
    } else {
      dv.setUint16(2, p.x, true);
      dv.setUint16(4, p.y, true);
      dv.setUint8(7, 0);
    }
    dv.setInt8(6, p.wheel);
    hidSend(buf);
  }

  function flushPendingIfOtherMode(m) {
    if (!pending || pending.mode === m) return;
    if (raf) {
      cancelAnimationFrame(raf);
      raf = 0;
    }
    flushMouse();
  }

  /* Coalesce moves to one WS frame per animation frame; flush immediately on
   * wheel or any button transition so clicks are never merged away. */
  function queueMouseAbs(buttons, x, y, wheel, forceImmediate) {
    flushPendingIfOtherMode("abs");
    const prev = pending;
    const btnChanged = prev !== null && prev.mode === "abs" && prev.buttons !== buttons;
    let w = wheel;
    if (prev && prev.mode === "abs") w = clampWheel(prev.wheel + wheel);
    pending = { mode: "abs", buttons, x, y, wheel: w };
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
      pending = { mode: "rel", buttons, dx, dy, wheel };
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

  function mapAbs(ev) {
    const r = canvas.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) return { x: 0, y: 0 };
    const x = Math.max(0, Math.min(ABS_MAX, Math.round(((ev.clientX - r.left) / r.width) * ABS_MAX)));
    const y = Math.max(0, Math.min(ABS_MAX, Math.round(((ev.clientY - r.top) / r.height) * ABS_MAX)));
    return { x, y };
  }

  function mouseButtons(ev) {
    let b = 0;
    if (ev.buttons & 1) b |= 1;
    if (ev.buttons & 2) b |= 2;
    if (ev.buttons & 4) b |= 4;
    /* mousedown: some engines lag updating `buttons`; use `button` for the activating click. */
    if ((ev.type === "pointerdown" || ev.type === "mousedown") && b === 0 && typeof ev.button === "number") {
      if (ev.button === 0) b |= 1;
      else if (ev.button === 1) b |= 4;
      else if (ev.button === 2) b |= 2;
    }
    return b;
  }

  function isMousePointer(ev) {
    return !ev.pointerType || ev.pointerType === "mouse" || ev.pointerType === "pen";
  }

  /* Tablet (absolute) mode: hover moves the host cursor, no capture needed. */
  canvas.addEventListener("pointermove", function (ev) {
    if (mode !== "abs" || pointerLockActive() || !isMousePointer(ev)) return;
    const { x, y } = mapAbs(ev);
    queueMouseAbs(mouseButtons(ev), x, y, 0, false);
  });

  canvas.addEventListener("pointerdown", function (ev) {
    if (!isMousePointer(ev)) return;
    if (mode === "abs") {
      canvas.focus(); /* keyboard capture; preventDefault below would block it */
      ev.preventDefault();
      try {
        canvas.setPointerCapture(ev.pointerId); /* keep drags outside the canvas */
      } catch (e) { /* pointer already gone */ }
      const { x, y } = mapAbs(ev);
      queueMouseAbs(mouseButtons(ev), x, y, 0, true);
      return;
    }
    if (ev.button === 0 && !pointerLockActive() && typeof canvas.requestPointerLock === "function") {
      ev.preventDefault();
      const req = canvas.requestPointerLock();
      if (req && typeof req.catch === "function") req.catch(() => { /* gesture policy */ });
    }
  });

  canvas.addEventListener("pointerup", function (ev) {
    if (mode !== "abs" || pointerLockActive() || !isMousePointer(ev)) return;
    ev.preventDefault();
    const { x, y } = mapAbs(ev);
    queueMouseAbs(mouseButtons(ev), x, y, 0, true);
  });

  canvas.addEventListener(
    "wheel",
    function (ev) {
      if (mode !== "abs" || pointerLockActive()) return;
      ev.preventDefault();
      const w = wheelStep(ev);
      if (w === 0) return;
      const { x, y } = mapAbs(ev);
      queueMouseAbs(mouseButtons(ev), x, y, w, true);
    },
    { passive: false },
  );

  canvas.addEventListener("contextmenu", (ev) => ev.preventDefault());

  /* Relative mode: while locked, events arrive on the document. */
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

  /* ---------------- audio (optional HDMI capture) ---------------- */

  /* Raw PCM S16LE 48 kHz stereo over /audio, played via an AudioWorklet ring
   * buffer (~250 ms capacity; underflow plays silence). Off until the user
   * clicks - browsers require a gesture to start audio anyway. */
  const AUDIO_WORKLET_SRC = `
    class P4KvmAudio extends AudioWorkletProcessor {
      constructor() {
        super();
        this.buf = new Float32Array(24000);
        this.r = 0; this.w = 0;
        this.port.onmessage = (e) => {
          const d = e.data;
          for (let i = 0; i < d.length; i++) { this.buf[this.w % this.buf.length] = d[i]; this.w++; }
          if (this.w - this.r > this.buf.length) this.r = this.w - this.buf.length;
        };
      }
      process(inputs, outputs) {
        const L = outputs[0][0], R = outputs[0][1] || outputs[0][0];
        for (let i = 0; i < L.length; i++) {
          if (this.w - this.r >= 2) {
            L[i] = this.buf[this.r % this.buf.length];
            R[i] = this.buf[(this.r + 1) % this.buf.length];
            this.r += 2;
          } else { L[i] = 0; R[i] = 0; }
        }
        return true;
      }
    }
    registerProcessor("p4kvm-audio", P4KvmAudio);`;

  let audioOn = false;
  let audioCtx = null;
  let audioWs = null;
  let audioNode = null;

  async function audioStart() {
    audioCtx = new AudioContext({ sampleRate: 48000 });
    const url = URL.createObjectURL(new Blob([AUDIO_WORKLET_SRC], { type: "application/javascript" }));
    await audioCtx.audioWorklet.addModule(url);
    URL.revokeObjectURL(url);
    audioNode = new AudioWorkletNode(audioCtx, "p4kvm-audio", { outputChannelCount: [2] });
    audioNode.connect(audioCtx.destination);
    audioWs = new WebSocket(proto + "://" + location.host + "/audio");
    audioWs.binaryType = "arraybuffer";
    audioWs.onmessage = function (ev) {
      const s16 = new Int16Array(ev.data);
      const f = new Float32Array(s16.length);
      for (let i = 0; i < s16.length; i++) f[i] = s16[i] / 32768;
      audioNode.port.postMessage(f, [f.buffer]);
    };
    audioWs.onclose = function () {
      if (audioOn) setHint("audio stream closed");
      audioStop();
    };
  }

  function audioStop() {
    audioOn = false;
    btnAudio.style.color = "";
    if (audioWs) { audioWs.onclose = null; audioWs.close(); audioWs = null; }
    if (audioNode) { audioNode.disconnect(); audioNode = null; }
    if (audioCtx) { audioCtx.close(); audioCtx = null; }
  }

  btnAudio.addEventListener("click", async function () {
    if (audioOn) { audioStop(); return; }
    try {
      await audioStart();
      audioOn = true;
      btnAudio.style.color = "var(--amber)";
    } catch (e) {
      setHint("audio start failed: " + e);
      audioStop();
    }
  });

  /* ---------------- fullscreen ---------------- */

  btnFullscreen.addEventListener("click", function () {
    if (document.fullscreenElement) document.exitFullscreen();
    else if (stageWrap.requestFullscreen) stageWrap.requestFullscreen();
  });

  /* ---------------- WebSocket lifecycle ---------------- */

  function connectWs() {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    ws = new WebSocket(proto + "://" + location.host + "/ws");
    ws.binaryType = "arraybuffer";
    ws.onopen = function () {
      refreshInputUi();
      updateHidToolButtons();
    };
    ws.onclose = function () {
      if (pointerLockActive()) document.exitPointerLock();
      setStatus("INPUT LINK DOWN", "err");
      setHint("input channel reconnecting…");
      updateHidToolButtons();
      reconnectTimer = setTimeout(connectWs, 2000);
    };
    ws.onerror = function () {
      ws.close();
    };
  }

  /* ---------------- boot ---------------- */

  setStatus("CONNECTING", "warn");
  startMjpegStream();

  /* Stagger startup requests: a freshly booted lwIP stack resets connections
   * when /stream, /jpeg-quality, /stats and /ws all race it at once. */
  let qualitySyncTimer = setTimeout(function () {
    qualitySyncTimer = null;
    syncJpegQualityFromDevice();
    syncMaxFpsFromDevice();
  }, 250);

  let initialWsTimer = setTimeout(function () {
    initialWsTimer = null;
    connectWs();
  }, 400);

  /* Upgrade to WebRTC (H.264 + data-channel HID) once the baseline is up. MJPEG
   * keeps painting until the video track goes live, so nothing shows black. */
  setTransportDiag();
  let webrtcTimer = setTimeout(function () {
    webrtcTimer = null;
    startWebrtc();
  }, 1200);

  let statsTimer = null;
  let statsKickoff = setTimeout(function () {
    statsKickoff = null;
    pollDeviceStats();
    statsTimer = setInterval(pollDeviceStats, 2000);
    loadConfigForm();
  }, 800);

  refreshInputUi();
  setHud(hudOn);

  if (import.meta.hot) {
    import.meta.hot.dispose(function () {
      painterRunning = false;
      if (painterWake) painterWake();
      if (streamAbortController) {
        streamAbortController.abort();
        streamAbortController = null;
      }
      for (const t of [qualitySyncTimer, initialWsTimer, statsKickoff, webrtcTimer]) {
        if (t) clearTimeout(t);
      }
      if (hidChannel) { try { hidChannel.close(); } catch (e) {} hidChannel = null; }
      if (pc) { try { pc.close(); } catch (e) {} pc = null; }
      if (statsTimer) clearInterval(statsTimer);
      if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
      }
      if (ws) {
        ws.onclose = null;
        ws.onerror = null;
        ws.close();
        ws = null;
      }
    });
  }
})();
