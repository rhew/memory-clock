# Simulator

Ubuntu deps:

```bash
sudo apt install build-essential cmake libsdl2-dev
```

Build and run:

```bash
cd sim
cmake -B build
cmake --build build
./build/mom-clock-sim
```

Config:

- Edit `sim/sample/config.json`
- Optional custom path: `./build/mom-clock-sim /path/to/config.json`

Current scope:

- LVGL SDL window on Linux
- Shared clock UI from `core/`
- Local JSON config injection for timezone, NTP, server URL, and bearer token
- Laptop network is allowed for future sim fetch work; ESP storage/network code is still separate
