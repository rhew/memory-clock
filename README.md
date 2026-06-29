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

You can build with a different env file:

```bash
idf.py -B build-local -DMEMORY_CLOCK_ENV_FILE=.env.local reconfigure build
```

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

## Local Server Testing

Put local server data under `server/local-data/`:

```text
server/local-data/calendar.yaml
server/local-data/devices.jsonl
```

`server/local-data/` is ignored by git. Use your existing test bearer token in
`server/local-data/devices.jsonl`; the firmware `BEARER_TOKEN` value must match
that device record.

Run the server on the host:

```bash
python3 server/clock_server.py \
  --host 0.0.0.0 \
  --port 8000 \
  --calendar server/local-data/calendar.yaml \
  --devices server/local-data/devices.jsonl
```

Run the server in Docker:

```bash
docker build -t memory-clock-server server && \
docker run --rm \
  -p 8000:8000 \
  -v "$PWD/server/local-data:/data:ro" \
  memory-clock-server
```

Use the test server from firmware with the LAN address of the machine running
the server:

```dotenv
CLOCK_SERVER_URL=http://192.168.x.y:8000/memory-clock
```

Do not use `127.0.0.1` for device testing; that points at the ESP32 itself.
Use `http` for local testing unless you specifically need to test TLS.

Example build and flash with local environment file:

```bash
idf.py -B build-local -DMEMORY_CLOCK_ENV_FILE=local-server.env reconfigure build flash monitor
```

## Build And Flash

```bash
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Run `idf.py reconfigure` before rebuilding if you:

- change `.env`
- change which env file you pass with `MEMORY_CLOCK_ENV_FILE`

For a local server test build:

```bash
idf.py -B build-local set-target esp32s3
idf.py -B build-local -DMEMORY_CLOCK_ENV_FILE=.env.local reconfigure build
idf.py -B build-local -p /dev/ttyUSB0 flash monitor
```

Example `.env.local`:

```dotenv
WIFI_SSID=your-ssid
WIFI_PASSWORD=your-password
BEARER_TOKEN=mc_your-existing-test-token
CLOCK_SERVER_URL=http://192.168.x.y:8000/memory-clock
CLOCK_POLL_INTERVAL_MS=5000
```

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
