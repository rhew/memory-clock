# `clock_server.py`

- Serves `GET /memory-clock`
- Requires `Authorization: Bearer <token>`
- Hashes the bearer token and matches it against `server/devices.jsonl`
- Reads `server/calendar.yaml`
- Sorts pages by date and drops entries before today
- Renders each calendar entry to a `400x480` 1-bit image
- Reloads `calendar.yaml` and `devices.jsonl` on each request
- Supports `If-Modified-Since` and returns `304 Not Modified` when neither calendar changes nor date-driven rendering changes have occurred
- Treats server startup as a content change so devices refresh after a restart

Returns JSON with:
- `tz`
- `ntp`
- `images` as XBM bit metadata with per-image raw bit paths
- `device` description for the matched token

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
  "device": "memory-clock"
}
```

## Container

Build the server image from `server/`:

```bash
docker build -t memory-clock-server .
```

The image installs the Python packages from `requirements.txt` and the Lato
font family used for rendering. It expects:

- `/data/calendar.yaml`
- `/data/devices.jsonl`

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
    networks:
      - reverse_proxy
    restart: unless-stopped
```

Example Caddy route:

```caddyfile
reverse_proxy /memory-clock* memory-clock:8000
```
