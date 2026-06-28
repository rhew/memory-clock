# memory-clock

ESP-IDF firmware for the Seeed Studio reTerminal E1001 (`800x480`, ESP32-S3).

Current firmware behavior:

- Connects to Wi-Fi using `WIFI_SSID` and `WIFI_PASSWORD` from `.env`
- Syncs time from `time.cloudflare.com`
- Uses New York Eastern time
- Renders a monochrome clock page with weekday, daypart, large 12-hour time, date, and the first image widget
- Renders additional image pages from files in `images/`, two images per page
- Uses the left and right front buttons to change pages, with wraparound
- Uses full refresh on page changes and 10-minute boundaries
- Uses partial refresh for minute changes while the clock page is visible

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

## Images

Image widgets live in `images/` as `*.xbm` files.

- Each image is compiled into the firmware
- The first image appears next to the clock
- Remaining images appear on later pages, two per page
- Image files should use a basename that is a valid C identifier, for example `page1.xbm`

If you add, remove, or rename image files, run `idf.py reconfigure` before rebuilding.

Use the left and right buttons to change the page. The reset button has no function yet.

## Build And Flash

```bash
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Run `idf.py reconfigure` before rebuilding if you:

- change `.env`
- add, remove, or rename files in `images/`

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
