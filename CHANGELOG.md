# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[semver](https://semver.org/) (pre-1.0: minor bumps may break).

## [0.2.0] - 2026-07-04

### Added
- Optional HDMI audio capture (`P4KVM_AUDIO_ENABLE`, h2c-rpi approach):
  TC358743 I2S output jumper-wired to three P4 GPIOs, received as I2S
  slave and streamed as raw PCM S16LE 48 kHz stereo over the /audio
  WebSocket; speaker toggle + AudioWorklet playback in the UI. Off by
  default; hardware-unverified until wired.
- Runtime video modes: 720p60 (default) and 1080p30, each with its own
  generated, checksummed EDID; selected from the web UI or POST
  /video-mode, persisted in NVS, applied via restart. HID absolute
  coordinates became resolution-independent (0..32767 on the wire) and
  the web canvas follows the device-reported frame size.
- WireGuard client (`P4KVM_WG_ENABLE`, trombik/esp_wireguard - same
  wireguard-lwip core as ciniml's Arduino port): key/endpoint config in
  menuconfig, waits for IP + NTP sync, supervises the handshake and
  reconnects when stale, state surfaced in /stats and the diagnostics
  panel. Tunnel throughput is software-crypto-bound (unmeasured).
- WiFi reliability: reconnect moved off the event-loop task onto a
  one-shot timer with exponential backoff (0.5-8 s), all-channel scan
  with strongest-BSS selection, in-supplicant quick retries, and power
  save re-asserted off after every association.
- WiFi station support (`P4KVM_WIFI_ENABLE`): the P4 has no radio, so the
  onboard companion chip (ESP32-C6 on the P4-nano) is driven over SDIO via
  esp_wifi_remote + esp_hosted. Coexists with Ethernet; mDNS announces on
  both; modem power save is disabled for input latency.
- Test-pattern mode (`P4KVM_TEST_PATTERN`): animated color bars generated in
  the selected pipeline's native pixel format, published through the same
  ring/semaphore contract as the CSI DMA - exercises BitScrambler, JPEG,
  streaming, UI and HID without the HDMI-CSI module attached.
- Alternative YUV422 capture pipeline (`P4KVM_PIPELINE_YUV422`): TC358743
  outputs UYVY (CSI-2 DT 0x1E, 4.1 MB/frame instead of 6.2 MB), consumed by
  the hardware JPEG encoder directly. Hardware-verified that the encoder's
  "YVYU" format id names the 32-bit word value - byte-wise it eats native
  UYVY, so no conversion stage exists. (A BitScrambler reorder pass built on
  the opposite assumption produced swapped chroma and measured ~28 MB/s /
  147 ms per 1080p frame; it was removed, main/uyvy_to_yvyu.bsasm remains as
  reference.) Output JPEG is 4:2:2. RGB888 (4:2:0 output) remains the
  default; compare with `/stats` on your network.
- USB HID absolute pointer ("virtual tablet", report ID 3, 0..32767 axes).
  Tablet mode is now the default in the web UI: the host cursor lands exactly
  where the browser pointer is, with no pointer lock and no drift from host
  pointer acceleration. Relative mode (pointer lock) remains for BIOSes/games.
- ATX front-panel control: `P4KVM_ATX_POWER_GPIO` / `P4KVM_ATX_RESET_GPIO`
  drive the host power/reset headers through an optocoupler or transistor.
  `POST /atx?op=power|power_hold|reset`; `power_hold` presses for 5 s (force
  off). Web UI buttons with confirmation prompts appear when configured.
- `/stats` JSON endpoint: capture fps, encode fps and time, BitScrambler
  reorder time, mean JPEG size, HDMI lock state, recovery count, heap. The web
  UI shows it as an overlay ("Show stats"), together with client-side draw fps
  and measured link Mbps.
- Optional HTTP Basic auth (`P4KVM_AUTH_ENABLE`, default off) on all routes
  including the /ws input WebSocket. Plain-HTTP Basic is an access hurdle,
  not transport security - keep using a VPN.
- Web UI: Ctrl+Alt+Del button, fullscreen button, keypad/PrintScreen/Pause
  key mapping, mode toggle, per-section settings panel.

### Changed
- Removed the 6 MB-per-frame cache msync in the encode loop (replaced by a
  one-time writeback+invalidate after buffer allocation; the JPEG and
  BitScrambler drivers handle per-run coherency themselves).
- `TCP_NODELAY` on the MJPEG stream and input WebSocket sockets: the small
  multipart trailer chunks no longer wait on delayed ACKs.
- JPEG output slot capacity raised from ~2.45 MB to 1.5 B/px (3.1 MB at
  1080p) so high quality settings and 4:2:2 scans cannot overflow.
- Web MJPEG client rewritten: incremental multipart parser that writes each
  JPEG byte exactly once into its final buffer (the old parser re-scanned and
  re-copied the whole buffer per network chunk), plus a decoupled decode loop
  that drops stale frames instead of queueing them - intended to cut
  glass-to-glass latency when the browser falls behind (not yet measured).
  The parser has a host-side test (chunk-split, resync, binary-body cases).
- Mouse absolute moves now map 1:1 via the HID absolute pointer instead of
  being emulated with relative segments.

### Fixed
- HDMI recovery ladder: when the source is unplugged (no DDC +5 V) the
  firmware waits instead of hammering HPD resets; after three failed hotplug
  recoveries it escalates to a full TC358743 register re-init (addresses the
  "source goes to sleep and p4kvm doesn't recover" known issue).
- Keyboard keys stuck on the host after the browser lost focus mid-keypress
  in tablet mode (all-keys-up is now sent on canvas blur and window blur).

[0.2.0]: https://github.com/DatanoiseTV/p4kvm/releases/tag/v0.2.0
