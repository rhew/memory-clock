# mom-clock

Local prototype server for the device screen endpoint.

## Requirements

```bash
python3 -m pip install Pillow
```

## Run

```bash
python3 server.py
```

The server listens on `127.0.0.1:8000` and serves `GET /screen.bmp`.

## Fetch The BMP

```bash
curl -o screen.bmp http://127.0.0.1:8000/screen.bmp
```
