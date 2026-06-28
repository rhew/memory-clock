# `clock_server.py`

- Serves `GET /clock`
- Requires `Authorization: Bearer <token>`
- Hashes the bearer token and matches it against `server/devices.jsonl`
- Reads `server/calendar.yaml`
- Sorts pages by date, drops entries before today, labels today as `Today`
- Renders each calendar entry to a `400x480` XBM page
- Reloads `calendar.yaml` and `devices.jsonl` on each request
- Supports `If-Modified-Since` and returns `304 Not Modified` when neither calendar changes nor date-driven rendering changes have occurred

Returns JSON with:
- `tz`
- `ntp`
- `images` as base64 XBM payloads
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
      "data_base64": "..."
    }
  ],
  "device": "memory-clock"
}
```
