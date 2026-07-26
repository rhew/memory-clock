# memory-clock

ESP-IDF firmware for the Seeed Studio reTerminal E1001 (`800x480`, ESP32-S3).

Current firmware behavior:

- Connects to Wi-Fi using `WIFI_SSID` and `WIFI_PASSWORD` from `env`
- Reconnects with backoff after an outage and can select a different access point or channel
- Syncs time from `TIME_SERVER` or `time.cloudflare.com` by default
- Uses `TIME_ZONE` or New York Eastern time by default
- Polls `CLOCK_SERVER_URL` for appointment page images
- Displays persistent admin messages over the full screen
- Renders a monochrome clock page with weekday, daypart, large 12-hour time, date, and the first appointment page
- Renders additional appointment pages two images per page
- Uses the left and right front buttons to change pages, with wraparound
- Uses the top green button to return to the first page or clear an admin message
- Uses full refresh on page changes and 10-minute boundaries
- Uses partial refresh for minute changes while the clock page is visible
- Shows unobtrusive status icons at the bottom of the clock pane only when a problem exists:
  Wi-Fi unavailable, clock server unavailable, or battery voltage low

## Status Icons

The clock page omits all status indicators while healthy. It displays the selected Tabler outline
icons in a right-aligned row only when their corresponding status is unhealthy.

- `wifi-off`: no Wi-Fi IP connection
- `cloud-off`: the last clock-server poll failed while Wi-Fi is connected
- `battery-exclamation`: battery voltage is at or below 3.50 V; it clears at 3.60 V

The E1001 battery monitor uses GPIO21 to enable its divider and GPIO1 for its ADC input. The
firmware samples it at startup and after each existing server poll; it does not add a polling task.
Each sample allows the divider to settle for 100 ms, uses the median of nine ADC readings, and
ignores impossible cell voltages so a bad read cannot show a false low-battery warning.

SVG sources, the Tabler MIT attribution, and the generated preview are under `assets/icons/`.
Regenerate the checked-in 1-bit C asset after changing an SVG or its rasterization settings:

```bash
make icons
```

This target requires `inkscape` and ImageMagick. Normal ESP-IDF builds use the checked-in generated
header and do not require those tools.

## IDF Setup

Install ESP-IDF and load it into your shell. For example, mine is installed in `~/.local`:

```bash
source ~/.local/lib/esp/esp-idf/export.sh
```

Verify the board:

```bash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 chip_id
```

Create `env` in the repo root. You can start from [`env.example`](./env.example):

Commented lines are the built-in defaults and can be omitted. Required values are:
`WIFI_SSID`, `WIFI_PASSWORD`, `BEARER_TOKEN`, and `CLOCK_SERVER_URL`.

You can build with a different env file:

```bash
idf.py -B build-local -DMEMORY_CLOCK_ENV_FILE=env.local reconfigure build
```

## Appointment Pages

The firmware fetches appointment pages from `CLOCK_SERVER_URL`.

- The request uses `Authorization: Bearer <BEARER_TOKEN>`
- The first server image appears next to the clock
- Remaining images appear on later pages, two per page
- After a successful fetch, later requests send `If-Modified-Since`
- Each primary page request also carries optional `X-Memory-Clock-*` telemetry headers. The server
  keeps only the latest battery voltage, seconds since the last button press, Wi-Fi RSSI, uptime,
  firmware version, and observation time for the authenticated clock.
- If the server returns changed pages, the firmware replaces the in-memory pages and redraws page 1
- If the server returns no images, the right widget says `No Appointments`
- If the first fetch fails before any pages load, the right widget shows the logo and a server error

Use the left and right buttons to change the page. Use the top button to return to page 1.

## Messages

The admin dashboard can queue one message for a specific clock. The message arrives during the
clock's existing server-poll cadence, covers the normal page, and remains until the top green
button is pressed. Page buttons do not dismiss or obscure it.

Each message can optionally select an alert sound defined by the server. The option defaults to
`None`; the supplied catalog includes a 1 kHz beep, the five-note “Shave and a Haircut” call,
and the first ten notes of “La Cucaracha.”
The clock repeats the selected sequence after each regular server-poll cycle while the message
remains active, including cycles that return `304 Not Modified`.

The clock reports the message lifecycle as `queued`, `displayed`, and `dismissed`. A dismissed
message's text is removed from the server database; only its identifier and timestamps remain
until another message replaces the row. The clock stores the pending dismissed identifier in NVS
so a power loss before the next poll cannot make a cleared message reappear.

Message state is sent only to `CLOCK_SERVER_URL`:

```text
X-Memory-Clock-Message-Capable: 1
X-Memory-Clock-Message-Active: 0123456789abcdef01234567
X-Memory-Clock-Message-Displayed: 0123456789abcdef01234567
X-Memory-Clock-Message-Dismissed: 0123456789abcdef01234567
```

The capability header lets the server leave older firmware on its normal `304 Not Modified`
cadence instead of repeatedly sending a queued message it cannot display. Removing an active
message in the admin dashboard causes capable firmware to clear it on its next poll.

The telemetry headers are sent only to `CLOCK_SERVER_URL`, not image URLs:

```text
X-Memory-Clock-Battery-Mv: 4128
X-Memory-Clock-Last-Interaction-S: 184
X-Memory-Clock-Wifi-Rssi: -49
X-Memory-Clock-Uptime-S: 9281
```

Unavailable values are omitted.

## Local Server Testing

Put local server data under `server/local-data/`:

```text
server/local-data/calendar.yaml
server/local-data/devices.jsonl
server/local-data/alerts.yaml
server/local-data/admin-token.sha256
```

`server/local-data/` is ignored by git. Use your existing test bearer token in
`server/local-data/devices.jsonl`; the firmware `BEARER_TOKEN` value must match
that device record.

Run the server on the host:

```bash
python3 server/create-admin-auth.py
python3 server/clock_server.py \
  --host 0.0.0.0 \
  --port 8000 \
  --calendar server/local-data/calendar.yaml \
  --devices server/local-data/devices.jsonl \
  --alerts server/local-data/alerts.yaml \
  --state server/local-data/memory-clock.sqlite3 \
  --admin-token-hash-file server/local-data/admin-token.sha256 \
  --allow-insecure-admin-cookie
```

Then open `http://127.0.0.1:8000/memory-clock/admin/` and select
`server/admin.token` on the sign-in page. The insecure-cookie option is only for local
HTTP testing; omit it behind the production HTTPS reverse proxy.

Run the server in Docker:

```bash
docker build -t memory-clock-server server && \
docker run --rm \
  -p 8000:8000 \
  -v "$PWD/server/local-data:/data:ro" \
  -v "$PWD/server/local-state:/state" \
  -e MEMORY_CLOCK_ADMIN_TOKEN_HASH_FILE=/data/admin-token.sha256 \
  -e MEMORY_CLOCK_ALERTS_PATH=/data/alerts.yaml \
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

Starting the monitor normally resets the ESP32 through the serial DTR/RTS lines. To attach to a
running clock without resetting it, use `idf.py monitor --no-reset`.

Run `idf.py reconfigure` before rebuilding if you:

- change `env`
- change which env file you pass with `MEMORY_CLOCK_ENV_FILE`

For a local server test build:

```bash
idf.py -B build-local set-target esp32s3
idf.py -B build-local -DMEMORY_CLOCK_ENV_FILE=env.local reconfigure build
idf.py -B build-local -p /dev/ttyUSB0 flash monitor
```

Example `env.local`:

```dotenv
WIFI_SSID=your-ssid
WIFI_PASSWORD=your-password
BEARER_TOKEN=mc_your-existing-test-token
CLOCK_SERVER_URL=http://192.168.x.y:8000/memory-clock
# CLOCK_POLL_INTERVAL_MS=300000
# TIME_SERVER=time.cloudflare.com
# TIME_ZONE=EST5EDT,M3.2.0/2,M11.1.0/2
# SNTP_SYNC_TIMEOUT_MS=15000
# WIFI_CONNECT_TIMEOUT_MS=30000
# BATTERY_LOW_MV=3500
# BATTERY_CLEAR_MV=3600
```

## Font Assets

Font files:
- `main/font_assets.c`
- `main/font_assets.h`

You can regenerate fonts before building if you want to change the typeface or sizes.
The default firmware font source is Lato Regular:
`/usr/share/fonts/truetype/lato/Lato-Regular.ttf`. CMake fails if that file is
missing. Set `MEMORY_CLOCK_FONT_FILE` only when regenerating the checked-in
font assets from a different source.

Example:

```bash
cc tools/generate_fonts.c $(pkg-config --cflags --libs freetype2) -O2 -o /tmp/memory-clock-fontgen
/tmp/memory-clock-fontgen /usr/share/fonts/truetype/lato/Lato-Regular.ttf main/font_assets.c main/font_assets.h
idf.py build
```
