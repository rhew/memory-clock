# reTerminal E1001 Details

This note captures the E1001 hardware facts that affect this firmware.

## Core Hardware

- MCU: `ESP32-S3`
- PSRAM on hardware: `8 MB`
- Flash on hardware: `32 MB`
- Display: `7.5"` monochrome ePaper
- Display resolution: `800 x 480`
- Wireless: `2.4 GHz 802.11 b/g/n Wi-Fi`, `Bluetooth 5.0`
- Battery: `2000 mAh`
- Storage expansion: `microSD`, up to `32 GB`, `FAT32`

## Display Hardware And Driver Assumptions

The firmware talks directly to the E1001 panel and its wiring.

- Controller: `UC8179`
- Current mode: `1-bit monochrome`
- The panel also supports `Gray4` on E1001 hardware, but this firmware does not use it.
- The panel is treated as `write-only`.
- ESP-IDF driver host: `SPI2_HOST`
- Display pin mapping in the app:
  - `SCLK = GPIO7`
  - `MOSI = GPIO9`
  - `CS = GPIO10`
  - `DC = GPIO11`
  - `RST = GPIO12`
  - `BUSY = GPIO13`
  - `MISO` is unused
- Driver assumptions:
  - `BUSY` stays active while the panel works
  - partial windows must align to `8-pixel` column boundaries
  - a full `1-bit` frame buffer uses `800 * 480 / 8 = 48,000 bytes`

## Buttons

The E1001 has three front buttons. Seeed names the signals `KEY0`, `KEY1`, and `KEY2`.

The vendor cookbook clearly documents the GPIOs:

- `KEY0 = GPIO3`
- `KEY1 = GPIO4`
- `KEY2 = GPIO5`

The physical left/right/top mapping in the vendor material is not clear enough to trust by itself. On the unit used for this firmware, observed behavior is:

- Left page button: `GPIO5`
- Right page button: `GPIO4`
- Top button: `GPIO3`

Other button facts that matter:

- All three buttons are `active-low`
- The hardware already provides pull-ups
- Press left and right together for `2 seconds` to reset network configuration

For this project:

- use left and right for page navigation
- leave the top button unused for now

## Wi-Fi

- The hardware supports `2.4 GHz` Wi-Fi only
- That limit matters for provisioning and connection troubleshooting

## Memory And PSRAM

Hardware reality:

- The E1001 includes `8 MB` PSRAM
- Seeed's cookbook describes this as roughly `7.9 MB` usable after runtime overhead in their Arduino examples

Current firmware reality:

- `PSRAM` is off in the current `sdkconfig`
- The app fits without PSRAM because it only keeps:
  - one full monochrome frame buffer
  - one previous frame buffer for change detection
  - small temporary regions for partial updates

Sizing that matters for future page/image work:

- Full-screen 1-bit image: about `48 KB`
- Half-screen 1-bit image: about `24 KB`
- A few pages of half-screen monochrome widgets are easy if PSRAM is enabled

Examples:

- `10` half-screen widgets: about `240 KB`
- `20` half-screen widgets: about `480 KB`
- `50` half-screen widgets: about `1.2 MB`

Project impact:

- Clock-only firmware does not need PSRAM
- Cached image widgets or multi-page pre-rendered layouts likely should use PSRAM

## Expansion Header

The rear `J2` header exposes useful signals for future extensions:

- `3.3V`
- `GND`
- `GPIO46`
- `GPIO2` / `ADC1_CH4`
- `GPIO17` / `UART TX`
- `GPIO18` / `UART RX`
- `GPIO20` / `I2C SCL`
- `GPIO19` / `I2C SDA`

This matters because the same `GPIO19/GPIO20` I2C pair is also the documented path for the onboard temperature/humidity sensor in Seeed's examples.

## Other Onboard Hardware Relevant To Future Work

The E1001 includes other peripherals that this firmware does not use yet:

- Onboard LED on `GPIO6`, with `inverted logic`
- Buzzer on `GPIO45`
- Temperature/humidity sensor on `I2C` (`GPIO19`/`GPIO20`)
- Microphone
- microSD on the shared SPI arrangement used by Seeed's examples
- Serial debug pins used in Seeed's Arduino examples: `GPIO43 TX`, `GPIO44 RX`

These peripherals matter if this project grows into:

- paged image/calendar content from SD
- low-battery or alert UX using the buzzer/LED
- environmental widgets
- additional serial diagnostics

## Not Currently Used

These hardware features are not wired into the app yet:

- refresh button handling
- buzzer
- LEDs
- sensors
- microphone
- Bluetooth
- SD card
- grayscale panel mode
- PSRAM

## Sources

- Seeed getting started:
  `https://wiki.seeedstudio.com/getting_started_with_reterminal_e1001/`
- Seeed Arduino peripherals cookbook:
  `https://wiki.seeedstudio.com/reterminal_e10xx_with_arduino_peripherals/`
