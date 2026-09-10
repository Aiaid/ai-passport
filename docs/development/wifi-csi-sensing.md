<p align="right">
  <a href="wifi-csi-sensing.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# ECHO — Wi-Fi CSI Sensing Mode

A passive Wi-Fi Channel State Information (CSI) sensing mode, added as a seventh
menu entry ("WiFi CSI") on the `feature/wifi-csi-sensing` branch. It is a benign
sensing application: the device receives packets and derives a motion score from
the channel response. It does not jam, attack, or decrypt other parties' traffic.

## What it does

- Connects as a 2.4 GHz Wi-Fi STA to the user's own router and pings the gateway
  at a fixed rate, so each echo reply yields a CSI record (receive only).
- Computes per-subcarrier amplitudes and an EWMA-based motion score on-device.
- Streams each CSI record as a CSV line over the USB-Serial-JTAG console for host
  analysis.
- Exposes a BLE GATT peripheral so a phone or browser can view live status and
  send control commands while Wi-Fi runs (the ESP32-C3 time-slices one 2.4 GHz
  radio between Wi-Fi and BLE; software coexistence is enabled).
- Optionally performs FTM (Fine Timing Measurement) ranging to the AP.

## Source files

| File | Role |
| --- | --- |
| `main/demo_csi.c` / `.h` | Mode body: LVGL screen, background task, CSI callback, command loop |
| `main/csi_metric.c` / `.h` | Pure C (no ESP-IDF): amplitude conversion + EWMA motion metric |
| `main/csi_proto.c` / `.h` | Pure C: build STATUS JSON, parse CONTROL JSON commands |
| `main/csi_ble.c` / `.h` | NimBLE GATT peripheral (STATUS notify + CONTROL write) |
| `main/demo_radio.c` / `.h` | NVS / netif / event setup and a blocking STA connect helper |
| `main/Kconfig.projbuild` | CSI credentials and timing options |

`csi_metric` and `csi_proto` are covered by host tests
(`tests/test_csi_metric.c`, `tests/test_csi_proto.c`) in `tools/validate.sh --static`.

## Configuration (menuconfig)

Credentials are never committed; set them locally in `sdkconfig`:

- `CSI_WIFI_SSID`, `CSI_WIFI_PASSWORD` — the user's own network (default empty; the
  screen shows "No WiFi: set in menuconfig" when empty).
- `CSI_PING_INTERVAL_MS` — ping cadence (default 100).
- `CSI_PING_TARGET` — ping target (default: DHCP gateway).
- `CSI_FTM_AUTO_INTERVAL_S` — automatic FTM period in seconds (default 0 = off).

## Serial CSV contract

One line per CSI record on the USB-Serial-JTAG console:

```
CSI_DATA,<rx_seq>,<timestamp_us>,<rssi>,<rate>,<noise_floor>,<channel>,<motion_0_100>,<csi_len>,[b0 b1 ... b(len-1)]
```

Header fields are comma-separated; the trailing bracket holds `csi_len`
space-separated `int8` values, interleaved `(imag, real)` pairs per subcarrier
(subcarriers = `csi_len / 2`; amplitude = `sqrt(real^2 + imag^2)`).

## BLE GATT contract

Advertised name: `AIPassport-CSI`

- Sensing service: `e2e90001-8f2a-4c7b-9f3d-1a2b3c4d5e6f`
- STATUS (Read + Notify) `e2e90002-...`: UTF-8 JSON ~3 Hz with fields `st`
  (0 off, 1 connecting, 2 running, 3 failed), `ssid`, `rssi`, `mot` (0-100),
  `rate` (CSI pkt/s), `ftm` (distance cm, 0 = invalid), `fv` (FTM valid 0/1).
- CONTROL (Write / Write Without Response) `e2e90003-...`: UTF-8 JSON commands
  `{"cmd":"start"}`, `{"cmd":"stop"}`, `{"cmd":"ftm"}`, `{"cmd":"ping_ms","v":100}`
  (v 20-2000). Commands drive capture start/stop, FTM, and the ping interval.

## FTM note

The ESP32-C3 is an FTM initiator. A distance estimate requires an AP that
advertises the FTM responder capability; many consumer routers do not. When the
AP is not a responder (or a session fails), the mode degrades gracefully: `fv`
stays 0 and the screen shows "Dist: N/A".

## Companion clients

Cross-platform clients (Python host tool, Web Bluetooth dashboard, iOS app) live
in the umbrella project's `companion/` directory, sharing the two contracts above.

## Verification status

- **Build**: PASS — ESP-IDF 5.5.3, app ~1.19 MB of the 3 MB partition; protected
  layout and merged image verified.
- **Host tests**: PASS — `csi_metric` and `csi_proto`.
- **Device tests**: NOT RUN (no hardware).

### Still requires on-device verification

1. RAM headroom under Wi-Fi + BLE coexistence (no PSRAM); link succeeded but heap
   use is a runtime figure.
2. FTM distance against an FTM-responder AP.
3. Repeated enter/exit stability, CSI packet rate under coexistence, and the full
   BLE path (connect, subscribe, notify, CONTROL commands) with the clients.
4. Motion sensitivity calibration (`alpha` / `scale` are empirical defaults).
