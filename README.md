# mom-clock

Prototype for a low-friction home clock/status display appliance.

## Shape

- Target device: Seeed Studio XIAO 7.5" ePaper Panel (`800x480`, ESP32-C3)
- Firmware direction: PlatformIO + ESP-IDF
- Device job: provision Wi-Fi, fetch data, render locally, cache last-good screen, report status
- Server job: decide content and serve one authenticated `/clock` document

## API

`GET /clock`

- Auth: `Authorization: Bearer <token>`
- Response: `200` JSON, plus `304` if conditional fetch is used
- Errors: `401`/`403` for auth failure, `5xx` for server failure

Example shape:

```json
{
  "time": {
    "timezone": "America/New_York",
    "ntp": ["time.cloudflare.com", "pool.ntp.org"]
  },
  "appointment_group": {
    "plan": "Freeda will pick you up at *10:00* in the morning.",
    "appointments": [
      {
        "start": "2026-06-24T08:45:00-04:00",
        "end": "2026-06-24T09:45:00-04:00",
        "text": "Consultation whith Dr. Who",
        "location": "Tardis"
      }
    ]
  },
  "messages": ["Drink water."]
}
```

Text may include limited inline markup: `**bold**`, `_italic_`.

## States

- `no_data`: show Wi-Fi setup instructions
- `unconnected_no_reliable_time`: show connection problem
- `connected`: show time, date, messages, appointments
- `unconnected_time_synced_since_power`: show time/date plus connection problem

## Add Device

Create a bearer token and store only its hash:

```bash
./add-device.py -d "Kitchen clock"
```

- Prints the token once for captive-portal entry
- Saves the hash with the device description
- Defaults to `devices.jsonl`; override with `-p <path>`

## Source Notes

Working notes live in [`prototype-docs/`](/home/rhew/repos/mom-clock/prototype-docs).
