# `clock_server.py`

- Serves `GET /memory-clock`
- Requires `Authorization: Bearer <token>`
- Hashes the bearer token and matches it against `server/devices.jsonl`
- Reads `server/calendar.yaml`
- Reads named alert sequences from the configured data directory
- Sorts pages by date and appointments by their 24-hour `HH:MM` start time
- Keeps today's page until one hour after its final appointment starts, then advances to the next
  appointment date; dates without appointments are skipped
- Labels the next appointment date `Tomorrow` when it is the following day, or `Next Appointment`
  when it is farther away
- Renders each calendar entry to a `400x480` 1-bit image
- Reloads `calendar.yaml` and `devices.jsonl` on each request
- Supports `If-Modified-Since` and returns `304 Not Modified` when neither calendar content nor a
  message state change needs delivery to capable firmware
- Advances `Last-Modified` at the daily appointment cutoff so polling clocks refresh the home page
- Treats server startup as a content change so devices refresh after a restart
- Records one current status snapshot per authenticated clock poll in SQLite, including `304` polls
- Serves an authenticated browser dashboard at `/memory-clock/admin/`
- Queues one persistent message per configured clock

Returns JSON with:
- `tz`
- `ntp`
- `images` as XBM bit metadata with per-image raw bit paths
- `device` description for the matched token
- `message` containing an active message or `null`

Response shape:

```json
{
  "tz": "EST5EDT,M3.2.0/2,M11.1.0/2",
  "ntp": "time.cloudflare.com",
  "images": [
    {
      "name": "page01.xbm",
      "mime_type": "image/x-xbitmap",
      "width": 400,
      "height": 480,
      "date": "2026-07-13",
      "label": "July 13",
      "encoding": "xbm-bits",
      "bits_path": "/memory-clock/images/page01.bin"
    }
  ],
  "device": "memory-clock",
  "message": {
    "id": "0123456789abcdef01234567",
    "text": "Dinner is ready!",
    "alert": {
      "tones": [
        {"frequency_hz": 1000, "duration_ms": 100, "gap_ms": 0}
      ]
    }
  }
}
```

## Admin Dashboard

The dashboard shows every configured clock and the latest information already sent by its current
firmware:

- human-readable last-seen time
- firmware version
- battery voltage
- Wi-Fi RSSI
- estimated start time
- last button-interaction time
- current message state

It does not retain telemetry history. Each poll atomically overwrites the device's single SQLite
row, and rows for devices removed from `devices.jsonl` are pruned when the dashboard is loaded.
Status recording is best-effort: an unavailable state database is logged but does not prevent a
clock from receiving pages.

Messages use one SQLite row per configured clock. Posting a new message replaces that row. The
row progresses through `queued`, `displayed`, and `dismissed`; dismissal immediately removes the
message text, so dismissed messages cannot be recalled. Messages are limited to 240 displayable
ASCII characters and remain active until the clock's top green button clears them or an
administrator removes them. Admin removal deletes the row and clears a displayed message on the
clock's next poll. Firmware advertises support with
`X-Memory-Clock-Message-Capable: 1`; queued messages do not force repeated responses to older
firmware that lacks that header.

The message dialog offers `None` by default plus the named sequences in `alerts.yaml`. When an
alert is selected, the server validates and snapshots its tones into the message row. Later edits
to the catalog therefore affect new messages without mutating an active one. Alert names identify
the choices in the configuration and admin API. The clock
safely replays its local snapshot after every regular server poll while the message remains
active, including unsuccessful polls and `304 Not Modified` responses.

Alert definitions contain a display name and up to 16 tones:

```yaml
alerts:
  - name: Beep
    tones:
      - frequency_hz: 1000
        duration_ms: 100
        gap_ms: 0
```

Frequencies must be 500–3000 Hz, tone durations 20–1000 ms, gaps 0–1000 ms, and the complete
sequence at most 5000 ms. `None` is built in and does not appear in the file. Copy
`alerts.example.yaml` to `local-data/alerts.yaml` when setting up the server.

The dashboard also provides authenticated previews of the effective appointment pages and a
read-only view of `calendar.yaml`. It never receives a device bearer token.

Create the administrator's browser token and corresponding server-side hash:

```bash
python3 create-admin-auth.py
```

Both files are created with mode `0600` and existing files are never overwritten. Keep them
private. The plaintext token is written to `admin.token` for selection on the browser sign-in
page; its server-side hash is written to `local-data/admin-token.sha256`. Configure the server with
the hash file:

```bash
python3 clock_server.py \
  --host 127.0.0.1 \
  --calendar local-data/calendar.yaml \
  --devices local-data/devices.jsonl \
  --alerts local-data/alerts.yaml \
  --state local-data/memory-clock.sqlite3 \
  --admin-token-hash-file local-data/admin-token.sha256 \
  --allow-insecure-admin-cookie
```

`--allow-insecure-admin-cookie` is strictly for loopback or trusted-LAN HTTP testing. Production
must use HTTPS and omit that option. The browser sends the admin token only during login, does not
put it in a URL or local storage, and then uses a 12-hour `HttpOnly`, `Secure`, `SameSite=Strict`
session cookie. Login attempts are rate limited. The page uses no third-party scripts and applies a
restrictive content security policy.

For non-browser API access, send the admin token directly as a bearer token to the admin API:

```text
Authorization: Bearer ma_...
```

Queue a message through the admin API:

```bash
curl -X POST https://example.test/memory-clock/admin/api/messages \
  -H 'Authorization: Bearer ma_...' \
  -H 'Content-Type: application/json' \
  -H 'X-Memory-Clock-CSRF: 1' \
  --data '{"device_id":"kitchen-clock","text":"Dinner is ready!","alert":"Beep"}'
```

Use `"alert":null` or omit it for no sound.

Remove a message or its dismissed-state record:

```bash
curl -X DELETE https://example.test/memory-clock/admin/api/messages/kitchen-clock \
  -H 'Authorization: Bearer ma_...' \
  -H 'X-Memory-Clock-CSRF: 1'
```

The admin token is independent of all device tokens. The server may alternatively read the hash
from `MEMORY_CLOCK_ADMIN_TOKEN_HASH_FILE` or directly from
`MEMORY_CLOCK_ADMIN_TOKEN_HASH`. Prefer a mounted hash file for container deployments.

New device records created by `add-device.py` receive a stable opaque `id`. Existing records
without one remain compatible and receive a deterministic, non-secret fallback ID; no firmware
change is required.

## Container

Build the server image from `server/`:

```bash
docker build -t memory-clock-server .
```

The image installs the Python packages from `requirements.txt` and the Lato
font family used for rendering. It expects:

- `/data/calendar.yaml`
- `/data/devices.jsonl`
- `/data/alerts.yaml`
- `/data/admin-token.sha256` when the dashboard is enabled
- a writable `/state` directory for `memory-clock.sqlite3`

Put `alerts.yaml` in the mounted data directory. The image includes `alerts.example.yaml` as a
starting point, but runtime alert configuration belongs under `/data` with the calendar, devices,
and administrator-token hash.

For an authenticated dashboard it also expects either the admin hash environment variable or a
mounted hash file. The browser-facing `admin.token` file does not need to be mounted into the
server.

It listens on port `8000` inside the container.

Mount the data directory, not individual files. Many editors save by renaming a
temporary file over the original; Docker file bind mounts can keep pointing at
the old inode.

## Server Deployment

The server handles `GET /memory-clock` and per-image paths under `/memory-clock/images/`.

Example `compose.yml` service:

```yaml
  memory-clock:
    build:
      context: ../memory-clock/server
    container_name: memory-clock
    volumes:
      - ./memory-clock-data:/data:ro
      - ./memory-clock-state:/state
    environment:
      MEMORY_CLOCK_ADMIN_TOKEN_HASH_FILE: /data/admin-token.sha256
      MEMORY_CLOCK_ALERTS_PATH: /data/alerts.yaml
    networks:
      - reverse_proxy
    restart: unless-stopped
```

Example Caddy route:

```caddyfile
reverse_proxy /memory-clock* memory-clock:8000
```

Keep the container port private to the Docker network so the HTTPS reverse proxy is the only public
path to the server. The default production session cookie is secure and will therefore be sent by
browsers only over HTTPS.
