# memory-clock

ESP-IDF firmware for the Seeed Studio reTerminal E1001 (`800x480`, ESP32-S3).

Current firmware behavior:

- Connects to Wi-Fi using ssid and password in `.env`
- Syncs time from `time.cloudflare.com`
- Uses New York Eastern time
- Renders a monochrome memory-clock layout:
  weekday, daypart, large 12-hour time, and long-form date
- Uses full refresh on 10-minute boundaries and partial refresh between minute changes

## IDF Setup

Install ESP-IDF and load it into your shell. For example, mine is installed in `~/.local`:

```bash
source ~/.local/lib/esp/esp-idf/export.sh
```

Verify the board:

```bash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 chip_id
```

Create `.env` in the repo root:

```dotenv
WIFI_SSID=your-ssid
WIFI_PASSWORD=your-password
```

## Build And Flash

```bash
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

If you change `.env` after the first configure, run `idf.py reconfigure` to update `wifi_env.h` before rebuilding.

## Font Assets

Font files:
- `main/font_assets.c`
- `main/font_assets.h`

You can regenerate fonts before building if you want to change the typeface or sizes.

Example:

```bash
cc tools/generate_fonts.c $(pkg-config --cflags --libs freetype2) -O2 -o /tmp/memory-clock-fontgen
/tmp/memory-clock-fontgen /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf main/font_assets.c main/font_assets.h
idf.py build
```
