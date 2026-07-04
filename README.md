# P4 KVM

This project uses an ESP32-P4 (rev < 3) and a Toshiba TC358743 HDMI to CSI adapter as a remote IP KVM:
1080p30 MJPEG video in the browser, USB HID keyboard, relative mouse and absolute pointer
("virtual tablet") toward the target, and optional ATX power/reset control.

## WARNING

Use this at your own risk. There is no TLS; the optional HTTP Basic auth is an access
hurdle, not transport security. Keep the device on a trusted network or behind a VPN.

## Parts Needed (affiliate links)

- Any [esp32-p4 module with rpi camera compatible CSI interface + ethernet](https://amzn.to/4v9c3Nf)
- [Toshiba TC358743 HDMI to CSI adapter](https://amzn.to/44mJyAp)

## 3d Printed Enclosure

This only works with the specific parts above:

[Makerworld](https://makerworld.com/en/models/2961981-esp32-p4-ip-kvm-enclosure)

## Building/Flashing Firmware

1. Use esp-idf 6.0.x
2. run menuconfig (select chip revision < 3, choose options in `P4KVM HDMI → CSI`)
3. idf.py build flash monitor (monitor is optional)
4. Open p4kvm.local in your browser

## Building Web App

1. cd web
2. npm install
3. npm run build (writes the bundled single-file UI into `main/index.html`)
4. follow build/flash steps above

## Using the UI

- **Tablet mode (default)**: move the mouse over the video and the host cursor follows
  1:1 (HID absolute pointer, no drift, no capture required). Click the video to also
  capture the keyboard; click outside to release it. Esc is forwarded to the host.
- **Relative mode**: click the video to grab the pointer (pointer lock). For BIOS
  setups, some KVM-unfriendly OS installers, and games that need relative motion.
  Esc releases the lock (browser behaviour, not configurable).
- **Settings (gear)**: Esc / Ctrl+Alt+Del / clipboard paste helpers, host power
  buttons (if wired, see below), JPEG quality, stats overlay, pointer sensitivity.
- **Stats overlay**: capture fps, encode fps and time, mean JPEG size, HDMI lock,
  plus browser-side draw fps and measured link rate. Also available as JSON at
  `GET /stats` for scripted A/B tuning.

## Video pipeline

The ESP32-P4 chip revision matters. This project assumes rev 1.x silicon
(rev 3+ adds H.264 encode and a CSI-bridge color converter; neither is used here).

Two build-time pipelines (`menuconfig → P4KVM → CSI capture pixel pipeline`):

- **RGB888 (default)**: TC358743 sends RGB888 (6.2 MB per 1080p frame over MIPI and
  into PSRAM), hardware JPEG encodes with 4:2:0 subsampling - the smallest files on
  the wire, which matters because the Ethernet PHY is 100 Mbit/s and is usually the
  throughput ceiling at 1080p30.
- **YUV422**: TC358743 sends UYVY (4.1 MB per frame, one third less MIPI/CSI-DMA
  bandwidth), fed directly to the JPEG encoder. Hardware-verified on rev 1.3 with
  the test pattern: the encoder's byte order is U Y V Y - native UYVY - because
  its "YVYU" format id names the little-endian 32-bit word value, not the byte
  sequence. (A BitScrambler reorder pass was built on that false premise; the
  color-bar test exposed it, and it also measured ~28 MB/s = 147 ms per frame -
  `main/uyvy_to_yvyu.bsasm` stays in the tree for reference only.) Output JPEG is
  4:2:2, measured ~260 KB/frame at quality 70 on the test pattern vs the smaller
  4:2:0 RGB888 output. Encode time measured 29 ms/frame at 1080p (360 MHz CPU,
  test pattern writing PSRAM concurrently).

Capture resolution is a runtime mode (drawer → Video → Resolution, or
`POST /video-mode?mode=720p60|1080p30`), persisted in NVS and applied with a
device restart, since changing it means advertising a different EDID to the
source and re-sizing every DMA buffer. Default is **720p60**: about one third
the MJPEG bitrate of 1080p at the same quality, which is what a 2.4 GHz WiFi
link realistically sustains; switch to 1080p30 on Ethernet for full detail.
1080p60 does not fit 2 MIPI lanes at this link rate even as YUV422.

**Verification status** (rev 1.3 board, WiFi, test pattern): boots, WiFi + mDNS +
HTTP + `/stats` + MJPEG streaming and the YUV422 encoder byte order are verified on
hardware; the multipart stream parser has a host-side test. NOT yet validated:
real HDMI capture through the TC358743, HID input against a live host, the
recovery ladder, ATX outputs, Ethernet, and auth-enabled operation.

## Test pattern mode (no HDMI-CSI module needed)

`menuconfig → P4KVM → Animated test pattern instead of HDMI capture`
(`P4KVM_TEST_PATTERN`) replaces the TC358743/CSI capture with CPU-generated
color bars plus a sweeping stripe, in the same resolution and pixel format as
the selected pipeline. Everything downstream - BitScrambler reorder, hardware
JPEG, HTTP streaming, the web UI, HID input - runs exactly as in capture mode,
so a bare ESP32-P4 board is enough to test the whole stack (and to verify the
YUV422 byte order visually: the bars must read white, yellow, cyan, green,
magenta, red, blue, black left to right).

## ATX power/reset control

Optional. Wire `P4KVM_ATX_POWER_GPIO` / `P4KVM_ATX_RESET_GPIO` to the target's
front-panel header **through an optocoupler (e.g. PC817) or an NPN transistor** -
never connect a GPIO to the header directly; the header pins are pulled up to the
host's standby rail, and grounds may differ. The default circuit expects
"GPIO high = button pressed" (`P4KVM_ATX_ACTIVE_HIGH`).

- UI "Power" / `POST /atx?op=power`: ~300 ms tap (boot, or request soft shutdown)
- UI "Force off" / `POST /atx?op=power_hold`: 5 s hold (ATX 4-second override)
- UI "Reset" / `POST /atx?op=reset`: ~300 ms tap

## Networking

- **Ethernet** (default): 100 Mbit/s RMII PHY, DHCP, mDNS (`p4kvm.local`).
- **WiFi station** (`P4KVM_WIFI_ENABLE`): the ESP32-P4 has no radio - boards like
  the P4-nano carry an onboard ESP32-C6 wired over SDIO, driven through
  `esp_wifi_remote` + `esp_hosted` (standard WiFi API, proxied to the C6). Set
  SSID/password in menuconfig; SDIO pins live in the esp_hosted component's own
  menuconfig section (defaults match Espressif's P4 reference design). Both
  interfaces can be up at once; mDNS announces on both. Mind the bitrate:
  1080p30 MJPEG at the default quality is roughly 40-70 Mbit/s, which a typical
  WiFi link may not sustain - drop the JPEG quality in the UI and watch the
  stats overlay's net rate.

## HDMI audio (optional)

`menuconfig → P4KVM → Enable HDMI audio capture` (`P4KVM_AUDIO_ENABLE`, off by
default). Same approach as h2c-rpi on the Raspberry Pi: the TC358743 outputs
the source's audio on its I2S pads (it is I2S master, clocks derived from the
HDMI stream; the audio path is already configured by this firmware). Jumper-
wire the adapter's I2S BCK / LRCK / DATA pads to three free P4 GPIOs and set
them in menuconfig. Audio streams as raw PCM S16LE 48 kHz stereo over the
/audio WebSocket (~1.5 Mbit/s); the speaker button appears in the top bar and
playback starts on click (browser gesture requirement). Sample rate is assumed
48 kHz - the only rate HDMI requires and the only one the EDID advertises.
Hardware-unverified until the I2S pads are actually wired.

## WireGuard

Built-in WireGuard client (`menuconfig → P4KVM → Enable WireGuard tunnel`,
lwIP implementation from trombik/esp_wireguard, the same core as ciniml's
WireGuard-ESP32-Arduino). Configure the interface private key, peer public key,
endpoint host/port, tunnel-local IP and optional preshared key; the tunnel comes
up after DHCP and an NTP time sync (handshakes need wall-clock time) and is
supervised - stale handshakes trigger an automatic reconnect. Diagnostics show
the tunnel state (`wg` in `/stats` and the WIREGUARD row in the panel). The KVM
is then reachable at its tunnel IP from anywhere the peer routes, which is the
intended remote-access path instead of exposing port 80. ChaCha20-Poly1305 runs
in software on the P4: expect tunnel throughput well below the MJPEG bitrate at
high quality - reduce JPEG quality accordingly (unmeasured; check `/stats` and
the link-rate readout over your tunnel).

## FAQ

### How do I access this remotely?

Use the built-in WireGuard client (above) or an external VPN (tailscale,
wireguard on your router, etc.) - do not expose this to the public internet.
`P4KVM_AUTH_ENABLE` adds HTTP Basic auth on top, but it runs over plain HTTP -
it does not replace the VPN.

### How do I change the resolution/framerate?

Drawer → Video → Resolution (720p60 / 1080p30); the device restarts into the
new mode. Other timings would need their own EDID - see video_mode.c and keep
the 2-lane MIPI budget in mind.

### Can I change the escape key from leaving captured input?

In relative (pointer lock) mode, no - browsers reserve Esc. Tablet mode has no lock,
so Esc is forwarded to the host there.

## Troubleshooting & known issues

- Cursor does not move in Tablet mode on BIOS/UEFI screens or bare EFI shells:
  many firmwares ignore HID absolute-pointer devices. Switch to Relative mode
  (pointer lock) for those.
- With auth enabled, /ws sends the browser's cached Basic credentials from the
  page load; a reverse proxy or the Vite dev proxy that strips the
  Authorization header on the upgrade breaks HID input while video still works.
- If the source drops HDMI (host sleep), the firmware now runs a recovery ladder:
  HPD hotplug cycles, escalating to a full TC358743 re-init after three failed
  attempts. If the source is unplugged (no DDC 5 V) it waits instead of cycling.
  If a host still comes back without video, check `/stats` (`hdmi_locked`,
  `recoveries`) and the serial log.
- some systems don't like the EDID, try changing the EDID
- try plugging in your target system before booting the p4 kvm
- try a different HDMI cable or USB cable
- look at the diagnostics for the tc358743: SYS_STATUS, i.e. (TMDS=1 HDMI=1 SYNC=1 DDC5V=1), they should all be 1

## Menuconfig (`P4KVM`)

- **`P4KVM_TC358743_RST_GPIO`**: reset line (active low); use `-1` if unwired (default GPIO 23).
- **`P4KVM_JPEG_QUALITY`**: 1-100. (Also available through the UI; persisted in NVS.)
- **`P4KVM_VIDEO_PIPELINE`**: RGB888 (default) or YUV422 + BitScrambler, see above.
- **`P4KVM_TEST_PATTERN`**: animated color bars instead of HDMI capture (no TC358743 needed).
- **`P4KVM_ATX_POWER_GPIO` / `P4KVM_ATX_RESET_GPIO` / `P4KVM_ATX_ACTIVE_HIGH`**: host power/reset buttons, `-1` = disabled.
- **`P4KVM_AUTH_ENABLE` / `P4KVM_AUTH_USER` / `P4KVM_AUTH_PASS`**: HTTP Basic auth (default off; empty password disables the check).
- **Ethernet**: `P4KVM_ETH_ENABLE` and RMII/MDIO/PHY GPIO options when `SOC_EMAC_SUPPORTED` applies.
- **WiFi**: `P4KVM_WIFI_ENABLE` / `P4KVM_WIFI_SSID` / `P4KVM_WIFI_PASSWORD` (see Networking).

## HTTP API

- `GET /` - web UI (single file)
- `GET /stream` - multipart MJPEG
- `GET /jpeg-quality[?q=1..100]` - read/set encoder quality
- `GET /stats` - JSON pipeline statistics
- `POST /atx?op=power|power_hold|reset` - front-panel buttons (404 if not wired)
- `WS /ws` - binary HID input (mouse/keyboard)
