# EleRelay — ESP32-2432S028 Smart Relay

An ESP32 firmware for the **ESP-2432S028** ("Cheap Yellow Display") board that fetches Estonian electricity spot prices from the [Elering NPS API](https://dashboard.elering.ee/assets/api-doc.html) and controls a relay based on the cheapest hours in a configurable window.

## Features

- Fetches 15-minute electricity price slots from the Elering API
- Turns relay ON during the N cheapest hours in a configurable look-ahead window
- ILI9341 320×240 colour display showing relay state, current price, and a price bar chart
- Captive-portal style web UI (no app needed) for all settings
- All settings stored in NVS — survive reboots
- **MQTT** publishing of current price and relay state
- Configurable NTP server and POSIX timezone string
- Fallback to AP mode ("ElereRelay-Setup") when WiFi credentials are missing or wrong

## Hardware

| Item | Details |
|------|---------|
| Board | ESP-2432S028 (ESP32-D0WD-V3, 4 MB flash) |
| Display | ILI9341 2.8″ 320×240 TFT (SPI, on-board) |
| Relay | Any 5 V relay module with active-LOW or active-HIGH IN |
| Default relay GPIO | GPIO 22 (configurable via `idf.py menuconfig`) |

### Wiring the relay

Connect your relay module's IN pin to the chosen GPIO (default GPIO 10), VCC to 5 V (or 3.3 V depending on the module), and GND to GND. Most common relay modules activate on LOW — keep **Relay activates on LOW** enabled in menuconfig.

## Getting started

### Prerequisites

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) installed and sourced
- Python 3 with `esptool` (included with ESP-IDF)

### Build

```bash
git clone https://github.com/kaar3l/EleRelay-ESP32-2432S028.git
cd EleRelay-ESP32-2432S028

# Optional: set compile-time defaults
idf.py menuconfig   # → Elering Smart Relay

idf.py build
```

### Flash

```bash
idf.py -p /dev/ttyUSB0 flash
```

Or with `esptool` directly (e.g. over RFC2217 remote serial):

```bash
python -m esptool --port rfc2217://192.168.x.x:4002 --baud 115200 --chip esp32 \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/elering_relay.bin
```

## First-time setup

1. Power on the board — it starts as a WiFi access point **ElereRelay-Setup** (open, no password).
2. Connect to that network and open `http://192.168.4.1/wifi`.
3. Enter your WiFi SSID and password and click **Save & Restart**.
4. The board connects to your network, syncs time via NTP, fetches prices, and starts the relay logic.

The IP address is shown on the display. Open `http://<ip>/` in a browser to see the price table.

## Web interface

| URL | Description |
|-----|-------------|
| `/` | Live price table with relay state |
| `/settings` | All runtime settings |
| `/wifi` | Change WiFi credentials |

## Settings

All settings are changed on the **`/settings`** page and take effect immediately without a reboot.

### Relay & Price

| Setting | Default | Description |
|---------|---------|-------------|
| Hours window | 12 h | Look-ahead window for cheap/expensive selection |
| Cheap hours | 6 h | How many of the cheapest hours to turn the relay ON |
| Inverted | off | When on, relay is ON during *expensive* hours instead |
| Fetch prices at | 23:00 | Hour of day for the daily price refresh (Elering publishes next-day prices ~14:00 EET) |
| Max slots on page | 48 | Maximum rows shown in the price table |

### Time

| Setting | Default | Description |
|---------|---------|-------------|
| NTP server | `pool.ntp.org` | Primary NTP server hostname |
| Timezone (POSIX TZ) | `EET-2EEST,M3.5.0/3,M10.5.0/4` | Standard POSIX TZ string — controls local time display and price slot alignment |

Common TZ strings:
- Estonia: `EET-2EEST,M3.5.0/3,M10.5.0/4`
- Finland: `EET-2EEST,M3.5.0/3,M10.5.0/4`
- Central Europe: `CET-1CEST,M3.5.0,M10.5.0/3`
- UTC: `UTC0`

### MQTT

When enabled, the device publishes to an MQTT broker on every relay update (once per minute).

| Setting | Default | Description |
|---------|---------|-------------|
| Enable MQTT | off | Turn MQTT publishing on/off |
| Server | *(empty)* | Broker hostname or IP address |
| Port | 1883 | Broker TCP port |
| Price topic | `elerelay/price` | Current slot price published as `c/kWh` (e.g. `12.345`) |
| Relay state topic | `elerelay/relay` | Relay state published as `ON` or `OFF` |

Both messages are published with **retain = 1**, so a newly connected subscriber always receives the latest value immediately.

Example with Mosquitto:
```bash
mosquitto_sub -h 192.168.1.10 -t 'elerelay/#' -v
# elerelay/price  8.720
# elerelay/relay  ON
```

## Display layout

```
┌──────────────────────────────────┐
│ EleRelay        Thu 16 Apr 13:42 │  ← header
├──────────────────────────────────┤
│ RELAY    CHEAP                   │
│  ON      8.7c                    │  ← relay status + current price
│          /kWh                    │
├──────────────────────────────────┤
│ WiFi: MyNetwork        6h/12h    │  ← WiFi info + window setting
├──────────────────────────────────┤
│ ▁▂▃█▇▅▄▃▂▁▂▃▄▅▆▇▅▄▃▂▁▂▃▄       │  ← price bar chart (green=cheap, red=expensive)
└──────────────────────────────────┘
```

## Compile-time defaults (menuconfig)

| Option | Default |
|--------|---------|
| WiFi SSID | `myssid` |
| WiFi Password | `mypassword` |
| Relay GPIO | 22 |
| Relay active LOW | yes |
| Hours window | 12 |
| Cheap hours | 6 |

These are only used on first boot if no NVS credentials/settings exist. All can be overridden at runtime via the web UI.

## License

MIT
