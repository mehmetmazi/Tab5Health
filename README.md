# M5Stack Tab5 — Health Dashboard (ESP32-P4)

Aggregates health data onto the Tab5's 5" 1280×720 display — a **5×3 grid of 15
live tiles** with a header status bar (clock, **Wi-Fi + iPhone-style battery
icons**), green/amber/red threshold colors, **7-day sparklines**, and **staleness
dimming**, auto-refreshing every 10 min. Idle 60 s → backlight sleeps; touch wakes it.
- **Oura:** sleep, readiness, HRV, resting HR, SpO2, respiratory rate, body temp, steps, activity
- **Withings:** weight, body fat, muscle, water %
- **Open-Meteo:** temperature + condition + feels-like + today's hi/lo, air quality (free, no key)
- **On-device:** battery (INA226), shown as a header status-bar icon

**ESP-IDF v5.4.2**, **M5GFX** (panel + touch), **LVGL 9** (UI), networking via the
**ESP32-C6** over esp-hosted (SDIO).

## Why M5GFX instead of the esp-bsp BSP

This unit's panel is an **ST7121** (touch FW v01). ESP-IDF's `esp_lcd` MIPI-DSI
driver / the `espressif/m5stack_tab5` BSP only know ILI9881C vs ST7123, so they
**mis-identify the ST7121 as ST7123** and the screen stays blank no matter the
init/timing (see [esp-bsp#695](https://github.com/espressif/esp-bsp/issues/695)).
M5GFX reads the touch FW version and drives the ST7121 correctly. LVGL renders
into M5GFX via a flush callback (`main/main.cpp`).

## Toolchain notes (important on this machine)

System Python is 3.14, which ESP-IDF v5.4.2 **rejects** (`requires >=3.10,<3.14`).
The toolchain runs on Homebrew's `python@3.12`, and `export.sh` derives its venv
from whatever `python3` is on `PATH`. Always enter the env via the helper:

```sh
. ~/esp/idfenv.sh        # python3.12 shim on PATH + source esp-idf/export.sh
```

- ESP-IDF: `~/esp/esp-idf` (v5.4.2). macOS host tools (`cmake ninja dfu-util ccache`)
  are from Homebrew — ESP-IDF doesn't bundle them on macOS.

## Build / flash / monitor

```sh
. ~/esp/idfenv.sh
cd ~/Code/M5Tab5
idf.py set-target esp32p4        # first time only
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

The Tab5 flashes over the ESP32-P4's native **USB-Serial-JTAG** (`/dev/cu.usbmodem*`,
VID 0x303A) — no driver, no manual download mode needed.

## Layout

```
sdkconfig.defaults    P4 config: PSRAM@200MHz XIP, 16MB flash, esp-hosted SDIO pins,
                      16KB main-task stack, LVGL9, TLS cert bundle
partitions.csv        16MB: 4MB app + 2MB SPIFFS
main/
  main.cpp            display + battery init, then a 10-min loop: connect → fetch
                      Oura + Withings → read battery → refresh tiles
  *_credentials.h     GITIGNORED secrets (wifi / oura / withings)
components/
  display/            M5GFX + LVGL bring-up; display_lock()/unlock(); LVGL task
  model/              HealthSnapshot (decoupled data struct) + Source enum
  ui/                 dark theme, 5×3 metric-card grid, header status bar (clock/
                      Wi-Fi/battery), per-tile sparklines (lv_chart), freshness dimming
  net/                esp-hosted (C6) Wi-Fi + SNTP + HTTPS helpers
  providers/          oura.cpp (PAT), withings.cpp (OAuth2 refresh + NVS),
                      weather.cpp (Open-Meteo, no key)
  battery/            INA226 over internal I2C (on-device tile 8)
```

## Data sources

- **Oura** — Personal Access Token (no OAuth). `main/oura_credentials.h`.
- **Withings** — OAuth2; do the browser dance once on a computer (local listener
  captures the code + exchanges it via `curl`), store the refresh token in
  `main/withings_credentials.h`. The device refreshes + rotates it (NVS).
- **Hilo (BP)** — *not wired*: Aktiia has no API; BP only reaches Apple Health.
  Would need a Health-Auto-Export → relay → device bridge (future work).

## Gotchas (the hard-won ones)

- **Display:** the flush callback must use `pushImage()`, not
  `setAddrWindow()`+`writePixels()` — the latter leaves a bottom garbage band on
  this rotated DSI panel. See `components/display/display.cpp`.
- **Networking boot-loop:** esp_hosted's `__attribute__((constructor))` auto-init
  starves DRAM → the 2nd-core idle stack lands in TCM → boot loop. Comment that
  call out and run `esp_hosted_init()` from `net_init()` instead. (Patch is in
  gitignored `managed_components/` — re-apply after a clean fetch.)
- **Withings** rotates the refresh token on every refresh — persist it to NVS.
- **Oura `end_date`** is timezone-exclusive of the *current* day — query **tomorrow**
  or today's row (e.g. today's steps) is silently dropped and every tile reads a day
  stale. `daily_activity` is the heavy endpoint (per-day 5-min MET series) → 96 KB
  parse buffer over the ~6-day window.
- **Main task stack** ≥16KB (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`) — TLS + JSON
  overflow the 3.5KB default (Stack protection fault).
- **INA226** is at **0x41** (not the driver default 0x40); the shared internal
  I2C bus wedges under network load — rebuild the bus on a failed read.
- **`app_main` must not return** — keep it in an infinite loop, or the idle-task
  stack overflows during cleanup.
