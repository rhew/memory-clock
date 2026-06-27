# memory-clock

ESP-IDF firmware for the Seeed Studio reTerminal E1001 (`800x480`, ESP32-S3).

Current firmware behavior:

- Connects to Wi-Fi using `.env`
- Syncs time from `time.cloudflare.com`
- Uses New York eastern time
- Renders a monochrome memory-clock layout:
  weekday, daypart, large 12-hour time, and long-form date
- Uses full refresh on 10-minute boundaries and partial refresh between minute changes

## Repo Shape

- The repo root is the ESP-IDF project
- Display code is in `main/display_port.c`
- Screen rendering is in `main/banner.c`
- Wi-Fi and SNTP startup is in `main/main.c` and `main/provisioning.c`

## IDF Setup

Install ESP-IDF and load it into your shell. For example, mine is installed in `~/.local`:

```bash
source ~/.local/lib/esp/esp-idf/export.sh
```

Verify the board:

```bash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 chip_id
```

## Build And Flash

```bash
idf.py set-target esp32s3
edit .env
idf.py reconfigure
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

If you change `.env` after the first configure, run `idf.py reconfigure` once before rebuilding so the generated `wifi_env.h` is refreshed.

## Checked-In Config

- `sdkconfig` is checked in because it is the known-good ESP-IDF config for the current E1001 firmware.
- `sdkconfig.defaults` is checked in because it seeds the build with the board's 32 MB flash size before `sdkconfig` exists.
