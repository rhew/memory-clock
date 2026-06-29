# memory-clock

ESP-IDF firmware for the Seeed Studio reTerminal E1001 (`800x480`, ESP32-S3).

Current firmware behavior:

- Connects to Wi-Fi using `WIFI_SSID` and `WIFI_PASSWORD` from `.env`
- Syncs time from `time.cloudflare.com`
- Uses New York Eastern time
- Polls `CLOCK_SERVER_URL` for appointment page images
- Renders a monochrome clock page with weekday, daypart, large 12-hour time, date, and the first appointment page
- Renders additional appointment pages two images per page
- Uses the left and right front buttons to change pages, with wraparound
- Uses the top button to return to the first page
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
BEARER_TOKEN=mc_your-token
CLOCK_SERVER_URL=https://rhew.org/memory-clock
CLOCK_POLL_INTERVAL_MS=300000
```

`CLOCK_POLL_INTERVAL_MS` defaults to 5 minutes when omitted.

## Appointment Pages

The firmware fetches appointment pages from `CLOCK_SERVER_URL`.

- The request uses `Authorization: Bearer <BEARER_TOKEN>`
- The first server image appears next to the clock
- Remaining images appear on later pages, two per page
- After a successful fetch, later requests send `If-Modified-Since`
- If the server returns changed pages, the firmware replaces the in-memory pages and redraws page 1
- If the server returns no images, the right widget says `No Appointments`
- If the first fetch fails before any pages load, the right widget shows the logo and a server error

Use the left and right buttons to change the page. Use the top button to return to page 1.

## Build And Flash

```bash
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Run `idf.py reconfigure` before rebuilding if you:

- change `.env`

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
