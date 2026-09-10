<p align="right">
  <a href="wifi-csi-sensing.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# ECHO — Passive Wi-Fi Sensing Firmware

ECHO is the passive Wi-Fi sensing application built on the `feature/wifi-csi-sensing`
branch. The host hardware is still the FoloToy AI Passport (ESP32-C3); ECHO is the
application/product name for this firmware build. The name is a nod to
echolocation: ECHO reads the human body's effect on Wi-Fi reflections instead of
emitting sound.

It has two sensing modes:

- **ECHO** — Channel State Information (CSI) sensing: a self-calibrating
  occupancy/motion "tripwire" on the link between the device and its access
  point (AP), plus optional FTM ranging.
- **RADAR** — passive device discovery: sniffs and coarsely classifies nearby
  Wi-Fi devices without connecting to anything.

Both modes are receive-only. The device never jams, attacks, or decrypts other
parties' traffic; RADAR does not associate with any network.

## Hardware

ESP32-C3, single antenna (1T1R), 2.4 GHz, HT20, ~64 subcarriers; 240×320 ST7789
RGB565 display with physically rounded corners; no PSRAM; 8 MB flash (3 MB
application partition, ECHO uses ~1.54 MB of it); ES8311 audio codec; three
buttons; USB-Serial-JTAG; BLE.

## Menu: ECHO / RADAR / Settings

The main menu has three cards: **ECHO**, **RADAR**, and **Settings**.

### ECHO (CSI sensing)

- Connects as a Wi-Fi STA to the user's own router and pings the gateway at a
  fixed rate; each echo reply yields one CSI record (~10 Hz).
- Computes an EWMA motion score from per-subcarrier CSI amplitude.
- **Self-calibrating occupancy tripwire**: on entering the mode, the device
  samples ~7 s of empty-room noise and derives a threshold automatically using
  robust statistics (median + median absolute deviation, MAD); it can be
  re-calibrated at any time (double-click OK, or the BLE `calib` command).
  Occupancy is decided from a **sliding-window median** of the motion score
  (anti-spike: an isolated empty-room motion spike does not dominate the
  window median and does not restart the "going empty" timer), and the screen
  shows OCCUPIED/EMPTY plus the current state's duration in seconds. The
  semantics are "did something cross the link between this device and its
  AP" — a tripwire, not room-wide presence — so the secondary status line
  reads "Link device-AP".
- Streams each CSI record as a CSV line over the USB-Serial-JTAG console for
  host analysis.
- UI: a half-circle motion gauge with a green→yellow→orange→red color ramp, a
  per-subcarrier amplitude heatmap bar, a status line, FTM distance, and an
  occupancy indicator.
- FTM (Fine Timing Measurement) ranging: the ESP32-C3 is an FTM initiator and
  needs an AP that supports the FTM responder role; otherwise it degrades
  gracefully to "N/A".

### RADAR (device discovery)

- Promiscuous/sniffing mode (never associates), hopping channels 1–13; a
  32-entry LRU device table.
- Classification: AP (from beacon frames), phone/PC/IoT (by OUI lookup), or
  Unknown (randomized MAC → UNKNOWN). The screen shows color-coded per-type
  counts (phone green, PC blue, IoT amber, AP purple, unknown gray) plus a
  running total, and a device list with a type badge, vendor name or MAC,
  RSSI, and an approximate-distance hint (`~rnd`).

### Settings (nine items, persisted to NVS)

Language (Chinese/English), occupancy threshold, smoothing (`alpha`), scale,
alert mode (off/once/continuous), volume, brightness, ping interval, and
recalibrate. Navigation uses the three buttons; changes apply live and persist
across power cycles.

## Light and sound alerts

An EMPTY→OCCUPIED transition (something crossed the tripwire) triggers a
buzzer — volume is adjustable, and the alert mode is off / once / continuous
(continuous repeats once every 1.5 s while occupancy holds) — together with a
BLE alert notification. A screen-flash alert was tried and removed based on
user feedback.

## BLE: always-on GATT peripheral (protocol v2)

The BLE GATT peripheral starts at boot and runs independently of the current
mode, advertised as `AIPassport-CSI`.

- Service: `e2e90001-8f2a-4c7b-9f3d-1a2b3c4d5e6f`
- **STATUS** (Read + Notify) `e2e90002-...`: UTF-8 JSON whose shape depends on
  the active mode.
  - Common: `st` (0 off, 1 connecting, 2 running, 3 failed), `mode`
    (`"echo"`/`"radar"`), `rate` (packet rate).
  - ECHO shape adds: `ssid`, `rssi`, `mot` (motion 0-100), `occ` (0/1),
    `occ_s` (occupancy duration, s), `ftm` (distance cm, 0 = invalid), `fv`
    (FTM valid 0/1), `alert` (a counter that increments on every tripwire
    trigger — clients detect a new alert by watching this value change).
  - RADAR shape adds: `dev` — `{ph, pc, io, ap, unk, tot}` per-type counts and
    total.
- **CONTROL** (Write) `e2e90003-...`: UTF-8 JSON commands. Supported: `start`,
  `stop`, `ftm`, `ping_ms` (v, ms, clamped 20-2000), `mode` (switch to
  `"echo"`/`"radar"`), `sens` (`alpha`/`scale`), `occ_th` (v, occupancy
  threshold), `alert` (v = 0/1, enable/disable), `alert_mode` (v = 0 off / 1
  once / 2 continuous), `vol` (v = 0-100), `bright` (v = 0-100), `calib`
  (re-run self-calibration against the current empty-room baseline).

## Serial CSV contract

One line per CSI record on the USB-Serial-JTAG console:

```
CSI_DATA,<rx_seq>,<timestamp_us>,<rssi>,<rate>,<noise_floor>,<channel>,<motion_0_100>,<csi_len>,[b0 b1 ... b(len-1)]
```

Header fields are comma-separated; the trailing bracket holds `csi_len`
space-separated `int8` values, interleaved `(imag, real)` pairs per subcarrier
(subcarriers = `csi_len / 2`; amplitude = `sqrt(real^2 + imag^2)`).

## i18n and the subset CJK font

The UI is bilingual (Chinese/English), defaulting to Chinese. On-device Chinese
text uses a **subset CJK font**: Noto Sans SC (OFL license), reduced to the
~74 Chinese characters actually used on screen, generated with `lv_font_conv`
as a compressed bitmap font (requires `CONFIG_LV_USE_FONT_COMPRESSED`). Every
translatable on-screen label goes through `ui_echo_font` (CJK font for
Chinese, `montserrat` for English), and switching language updates the screen
in place without recreating it. If Chinese label text changes, the font must
be regenerated; `tools/check_cjk_font.py` self-checks that every character
used in the Chinese strings is present in the generated font.

## UI visual style

A full-color ECHO HUD: dark-blue background with an amber brand accent, and
information encoded by color throughout (the motion color ramp, the
subcarrier heatmap, and RADAR device-type colors). The layout keeps a rounded
safe area for the physically rounded display: content insets 10 px from the
edges, with roughly 18 px of clearance reserved at each corner.

## Source files

| File | Role |
| --- | --- |
| `main/demo_csi.c` / `.h` | ECHO mode body: LVGL screen (motion gauge, heatmap, occupancy indicator), background task, CSI callback, command loop |
| `main/demo_discovery.c` / `.h` | RADAR mode body: passive sniffing/channel hopping, device table, classification, LVGL screen |
| `main/demo_settings.c` / `.h` | Settings screen: the nine settings items, live-apply, NVS persistence |
| `main/occupancy.c` / `.h` | Pure C occupancy/tripwire state machine: sliding-window median, debounce/hysteresis (separate enter/leave timers), self-calibration from baseline samples (median + MAD) |
| `main/csi_metric.c` / `.h` | Pure C (no ESP-IDF): amplitude conversion + EWMA motion metric |
| `main/csi_proto.c` / `.h` | Pure C: BLE wire protocol — build STATUS JSON, parse CONTROL JSON commands (protocol v2) |
| `main/csi_ble.c` / `.h` | Always-on NimBLE GATT peripheral: advertising, STATUS notify timer, CONTROL command dispatch |
| `main/echo_state.c` / `.h` | Cross-task shared state: current status snapshot for BLE STATUS, mode-switch requests, and the ECHO command queue, all mutex/queue-guarded |
| `main/ui_i18n.c` / `.h` | Pure C bilingual (EN/ZH) string table for static on-screen labels |
| `main/ui_echo.c` / `.h` | Shared HUD palette, safe-area constants, and widgets reused by the menu, ECHO, and RADAR screens |
| `main/app_settings.c` / `.h` | Settings persistence: defaults, NVS load/save, apply-to-runtime, capture-from-runtime |
| `main/lv_font_echo_cjk_16.c`, `lv_font_echo_cjk_20.c` | Subset CJK bitmap fonts (generated, compressed) |

`csi_metric`, `csi_proto`, and `occupancy` are pure C with no ESP-IDF
dependency and are covered by host tests (`tests/test_csi_metric.c`,
`tests/test_csi_proto.c`, `tests/test_occupancy.c`) in
`tools/validate.sh --static`.

## Configuration (menuconfig)

Credentials are never committed; set them locally in `sdkconfig` under
`ECHO (WiFi CSI Sensing)`:

- `CSI_WIFI_SSID`, `CSI_WIFI_PASSWORD` — the user's own network (default
  empty; ECHO mode shows an unconfigured prompt and waits rather than
  crashing when empty).
- `CSI_PING_INTERVAL_MS` — ping cadence, 10-10000 ms (default 100).
- `CSI_PING_TARGET` — ping target IPv4 (default empty = use the DHCP
  gateway).
- `CSI_FTM_AUTO_INTERVAL_S` — automatic FTM period in seconds, 0-600 (default
  0 = off; FTM otherwise only runs on the BLE `ftm` command).

## Capability boundaries (stated honestly)

A single antenna means **no angle-of-arrival or direction**: presence and
motion detection are usable, but breathing detection, gesture recognition,
and precise localization are research-grade and are not implemented here.
Finding an AP's direction is only possible by walking around with the device
and reading RSSI strength — a coarse method. FTM requires an AP that supports
the responder role. RADAR mostly reports Unknown devices because modern
clients randomize their MAC address.

## Companion clients

Cross-platform clients (in the umbrella project's `companion/` directory)
consume the two contracts above:

- **host-csi**: a Python (`uv`) tool that reads the CSV stream from the
  serial port for a live subcarrier heatmap and motion curve, and offline
  analysis.
- **web**: a Web Bluetooth control panel (Chrome/Edge, desktop or Android;
  not iOS; requires `localhost` or HTTPS) that consumes GATT protocol v2 —
  occupancy, device counts, mode, sensitivity, alerts, recalibration, and an
  alert toast.
- **ios**: a SwiftUI + CoreBluetooth iPhone app (home-screen name ECHO,
  requires a paid Apple Developer account) with the same feature set.

## Verification status

- **Build**: PASS — ESP-IDF 5.5.3, app ~1.54 MB of the 3 MB partition;
  protected layout and merged image verified.
- **Host tests**: PASS — all host-testable logic (`csi_metric`, `csi_proto`,
  `occupancy`, and related pure-C modules).
- **Device tests**: verified on real hardware — clean boot; CSI at ~10
  packets/s with a live motion response; self-calibrated occupancy tripwire
  transitioning EMPTY/OCCUPIED correctly; light-and-sound alert (buzzer)
  firing on tripwire crossing; RADAR discovery (AP and unknown-device counts
  updating); BLE STATUS refreshing over a live connection; the Chinese UI,
  including language switching without a hang and a clean layout.

### Still requires on-device verification

1. FTM distance against a real FTM-responder AP.
2. Long-running Wi-Fi + BLE coexistence stability.
3. iOS and Web companion clients end-to-end against real hardware.
