# MacroPad 3

First of all - credits to the author of the original project: Kamil Sokalski. Project presented here: https://www.youtube.com/watch?v=nof_qYYGMVU.
I just did not like original firmware, and that is my rework of it to match my requirements.

ESP-IDF firmware for a programmable 9-key macropad with a rotary encoder, built on the **Waveshare ESP32-S3-Zero**. It works as a USB keyboard or a Bluetooth LE keyboard, and is configured from a web page served by the macropad itself.

It replaces an earlier Arduino sketch. The pin assignment is the same, so existing hardware runs it unchanged.

> **Using the macropad rather than building it?** See the [User manual](docs/USER_MANUAL.md).

## Features

- **9 keys + rotary encoder.** The encoder's left turn, right turn and push button each count as a key, so 12 inputs can be mapped.
- **9 profiles.** Each has its own name, LED colour, scan rate and 12 key mappings. Switch between them from the encoder, without a computer.
- **What a key can do:**
  - a key or shortcut: any modifiers plus one key, held while the physical key is held
  - a media key: volume, playback, brightness, launchers, browser keys
  - type a text: up to 47 characters
  - a power key: power off, sleep, wake up
- **Fast, reliable scanning.** A hardware-timer interrupt samples the keys at 1, 2, 4, 8 or 16 kHz. Debouncing reports a change on the first sample, so the delay from contact to detection is at most one scan period. The encoder is decoded by the PCNT hardware counter.
- **USB first, Bluetooth otherwise.** USB HID polls every 1 ms. Bluetooth LE HID asks for a 7.5–15 ms connection interval. While a USB host is attached, Bluetooth is switched off entirely.
- **Status LED** in the active profile's colour; the pattern shows connection state and modes.
- **Web UI**, a single page, with:
  - a graphical editor: 3×3 keypad and encoder knob, full keyboard image, media deck
  - live key test and latency estimate
  - device settings, up to 5 Wi-Fi networks, config export and import
  - over-the-air firmware update
- **Physical-presence check for the web UI.** The page shows 4 random keys to press on the macropad before it allows anything, and sessions end after 10 minutes of inactivity.

## Hardware

| Function | GPIO | Notes |
|---|---|---|
| Keys 1–9 | 1, 2, 3, 4, 5, 6, 7, 8, 17 | Key 1 held at power-up = Wi-Fi setup mode |
| Encoder A / B | 9 / 10 | Quadrature, decoded by PCNT |
| Encoder push switch | 38 | |
| Status LED | 21 | On-board WS2812, R-G-B byte order |
| Log output (UART0) | 43 TX / 44 RX | 115200 baud |

All inputs are active-low with the chip's internal pull-ups; switches connect the pin to GND. The pin map is in [main/board.h](main/board.h). The keys are assumed to be laid out 1 2 3 / 4 5 6 / 7 8 9, top-left to bottom-right; the web UI and the verification keypad draw them that way.

Optional: wire a USB VBUS sense pin (through a divider) and set `BOARD_VBUS_SENSE_PIN`. Then unplugging USB is detected even when the board has another power source. See [Known limitations](#known-limitations).

## Building and flashing

Requirements: **ESP-IDF 5.4** (tested with v5.4), target `esp32s3`. The component manager fetches `esp_tinyusb` 1.7.x, `led_strip` 2.5.x and `mdns` on the first build.

```powershell
idf.py set-target esp32s3   # once
idf.py build
idf.py -p COMx flash
```

### Flashing with ESP Web Tool

When a firmware release is available, download its `.zip` file from the [latest GitHub release](https://github.com/boa-pe/MacroPad3BoaRework/releases/latest) and unzip it.
Open the [ESP Web Tool](https://esptool.spacehuhn.com/) in a desktop Chromium-based browser, connect the board, and select its serial port. Add each `.bin` file and enter the corresponding address from `flash-addresses.txt`, then click **Program**. Keep the board connected until flashing completes.

**After the first flash, the native USB port is the keyboard.** The USB serial/JTAG port is gone, so later updates go one of two ways:

1. **Over the air:** open the web UI → *Status & maintenance* → *Firmware update* → upload `build/macropad3.bin`. Two OTA slots of 1.875 MB each; the current image is about 1.1 MB.
2. **Download mode:** hold **BOOT** while plugging the board in, then run `idf.py -p COMx flash`.

### Partition table

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB |
| otadata | data/ota | 0xF000 | 8 KB |
| phy_init | data/phy | 0x11000 | 4 KB |
| ota_0 | app | 0x20000 | 1.875 MB |
| ota_1 | app | 0x200000 | 1.875 MB |

## First start

1. With no Wi-Fi network saved, the macropad opens an access point named **`MacroPad-XXXX`**, and the LED blinks slowly.
2. Join it from a phone or laptop. A captive portal opens `http://192.168.4.1`.
3. Press the 4 keys the page shows (the physical-presence check), add your Wi-Fi network(s), and save. The macropad reboots and joins the network.
4. From then on the UI is at **`http://macropad.local`**, or at the IP address your router assigned.

The setup access point starts **only** when no network is saved, or when **key 1 is held while plugging in**.

## Architecture

```
 keys ──► GPTimer ISR (1–16 kHz, eager debounce) ─┐
                                                  ├─► queue ─► keyproc task (prio 21, core 1)
 encoder ─► PCNT (glitch filter, watch points) ───┘           │  profile/Fn logic, text typing
                                                              ▼
                                         hid_out ──► USB HID (TinyUSB, 1 ms poll)
                                                 └─► BLE HID (NimBLE, core 0) when no USB host
 config (NVS, deferred writes) ◄── web_server (esp_http_server, core 0) ◄── Wi-Fi STA / setup AP
```

- **Scanning:** `scanner.c`. The ISR reads `GPIO_IN_REG`/`GPIO_IN1_REG` directly and timestamps each event for the latency estimate. It is IRAM-safe, so it keeps running while flash is written (NVS, OTA). Keys already held at power-up are ignored until released.
- **Key processing:** `keyproc.c`.
  - ACT_KEY and ACT_MEDIA follow the physical key: press on press, release on release. Host auto-repeat therefore works.
  - Encoder detents send a press followed by a release.
  - ACT_TEXT types its string once, on press.
  - The encoder button sends its own action on a short press (on release). A long press of 1.5 s enters profile selection.
- **One HID report map for USB and BLE:**
  - report 1: keyboard, 6KRO, plus an LED output report
  - report 2: consumer control
  - report 3: system control (power / sleep / wake)
- **Link management:** `hid_out.c`.
  - At boot it waits up to 1.5 s for USB enumeration and only then starts BLE.
  - Afterwards BLE follows USB: when a host mounts the device, BLE stops advertising and drops its connection; when the host unmounts it, BLE comes back.
  - The NimBLE stack itself stays initialised.
- **Wi-Fi:** `wifi_mgr.c`.
  - Up to 5 saved networks. Each attempt scans first and tries saved networks strongest first; unseen (hidden) networks are tried last.
  - When nothing connects, it backs off 1 → 60 s, so the shared 2.4 GHz radio stays free for Bluetooth.
  - Modem sleep is on, which Wi-Fi/BT coexistence requires.
- **Config:** `config.c`.
  - NVS namespace `macropad` holds blobs `global`, `p0`–`p8` (one per profile) and `wifi`.
  - Writes are coalesced and land about 1.5 s after the last change.
  - A config saved by the older single-network firmware is migrated automatically.

### Latency

The status page shows an estimated key-to-host latency for the active link. It combines a measurement with a model:

- **Measured on the device:** time from the ISR detecting the key to
  - USB: the report being collected by the host (TinyUSB transfer complete), or
  - BLE: the notification being queued in the Bluetooth stack.

  The last 32 presses per link are kept.
- **Modelled:**
  - the wait from contact to the next scan sample: ½ scan period on average, 1 period at most
  - for BLE, the wait for the next connection event: ½ interval on average, 1 interval at most

USB on the ESP32-S3 is full-speed only, so the host polls at most every 1 ms. Scan rates above 1 kHz still help: the press is detected earlier, so it more often makes the next poll.

## Web API

All routes are on port 80. Routes marked 🔒 need a session cookie (`mp_session`), obtained through the key-sequence check. Background polling of `/api/status` doesn't extend a session's idle timer.

| Method & path | 🔒 | Purpose |
|---|---|---|
| `GET /` | | The single-page UI (holds no data) |
| `GET /api/session` | | `{"authed":bool}` |
| `POST /api/auth/start` | | New check: `{"id","keys":[4 key indexes 0–8],"ttl":60}` |
| `GET /api/auth/status?id=` | | `pending` / `ok` / `failed` / `expired` / `cancelled`; on `ok` it sets the cookie |
| `POST /api/auth/cancel?id=` | | Cancel a pending check |
| `POST /api/logout` | 🔒 | End the session |
| `POST /api/ping` | 🔒 | Report user activity (restarts the 10-minute idle timer) |
| `GET /api/config` | 🔒 | Global settings, Wi-Fi network names, all profiles |
| `POST /api/profile?i=0..8` | 🔒 | Update a profile (partial objects allowed; `{"reset":true}` restores defaults) |
| `POST /api/global` | 🔒 | Device settings |
| `POST /api/active` | 🔒 | `{"active":n}` switches the profile |
| `GET /api/status` | 🔒 | Link state, pressed keys, scan rate, encoder counts, Wi-Fi, latency estimate |
| `GET /api/scan` | 🔒 | Wi-Fi scan results |
| `POST /api/wifi` | 🔒 | `{"networks":[{"ssid","pass"?}…]}`, max 5; leaving out `pass` keeps the stored one |
| `POST /api/ble/forget` | 🔒 | Delete all Bluetooth pairings |
| `POST /api/reboot` | 🔒 | Reboot |
| `POST /api/factory` | 🔒 | Erase settings and pairings, then reboot |
| `POST /api/ota` | 🔒 | Raw application `.bin` as the request body |

Key actions in the JSON look like `{"t":type,"m":modifiers,"k":keycode,"u":usage,"s":"text"}`:
- `t`: 0 none, 1 key, 2 media, 3 text, 4 system
- `m`: HID modifier bits
- `k`: HID keyboard usage
- `u`: consumer usage, or 0x81–0x83 for system keys

## Security notes

- The web UI is plain HTTP. The key-sequence check proves physical access, but someone who can read your Wi-Fi traffic could copy a session cookie.
- Anyone on the network can *start* a check, which captures the keys for up to 60 s. A pending check can be replaced only after 3 s, the encoder button cancels it, and a hidden or closed tab cancels its own.
- Sessions are kept in RAM: at most 4 at a time, ended after 10 minutes of inactivity, by **Lock**, or by a reboot.
- Bluetooth uses "just works" pairing with bonding (no PIN), the usual choice for keyboards without a display.

## Known limitations

- **No VBUS sense on the S3-Zero.** "USB connected" means "enumerated by a host". Unplugged from a PC while powered from elsewhere, the board can't tell that apart from a sleeping host, so Bluetooth stays off. A charger never enumerates, so Bluetooth works on a charger.
- **6KRO:** at most 6 keys plus modifiers at once.
- **Text typing** assumes a US keyboard layout on the host.
- **Changing the Bluetooth report layout** (for example, the update that added power keys) requires removing and re-pairing the macropad on the computer, because hosts cache the old layout.

## Source layout

| File | Purpose |
|---|---|
| [main/main.c](main/main.c) | Boot sequence, setup-mode decision, config change hooks |
| [main/board.h](main/board.h) | Pin map |
| [main/scanner.c](main/scanner.c) | GPTimer scan ISR, eager debounce, PCNT encoder, event timestamps |
| [main/keyproc.c](main/keyproc.c) | Events → HID reports, profile selection mode, text typing |
| [main/hid_out.c](main/hid_out.c) | Shared report map, USB/BLE routing, "USB attached → BLE off" |
| [main/usb_hid.c](main/usb_hid.c) | TinyUSB descriptors and callbacks, remote wake-up |
| [main/ble_hid.c](main/ble_hid.c) | NimBLE HID-over-GATT service, advertising, bonding |
| [main/latency.c](main/latency.c) | Runtime latency measurement and estimate |
| [main/auth.c](main/auth.c) | Key-sequence check and web sessions |
| [main/config.c](main/config.c) | Profiles, settings and Wi-Fi list in NVS |
| [main/status_led.c](main/status_led.c) | WS2812 patterns |
| [main/wifi_mgr.c](main/wifi_mgr.c) | Multi-network station, setup AP, scanning, mDNS |
| [main/dns_server.c](main/dns_server.c) | Captive-portal DNS |
| [main/web_server.c](main/web_server.c) | REST API, sessions, OTA |
| [main/web/index.html](main/web/index.html) | The web UI (embedded into the firmware) |
