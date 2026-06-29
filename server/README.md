# `clock_server.py`

- Serves `GET /clock`
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
      "date": "2026-06-29",
      "label": "June 29",
      "encoding": "xbm-bits",
      "bits_path": "/clock/images/page01.bin"
    }
  ],
  "device": "memory-clock"
}
```
