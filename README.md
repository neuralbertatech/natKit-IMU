# natKit-IMU

ESP32-based IMU sensor device for the natKit BCI toolkit. This device collects inertial measurement data and streams it to the natKit backend via MQTT.

## There are two firmwares — read this first

This repository holds **two** node firmwares. They speak to the same broker and
the same topic names, so knowing which one is on a board is not optional.

| | [`embeded/`](embeded) | [`firmware-idf/`](firmware-idf) |
|---|---|---|
| What | the firmware in use — every node is a full WiFi/MQTT/NTP client | a **fork** (EPIC TEC-NATKIT-20): primary/secondary nodes over ESP-NOW, serial uplink to one networked gateway |
| Framework | Arduino via pioarduino (arduino-esp32 3.3.11 / ESP-IDF 5.5.5) | native ESP-IDF (`idf.py`), v5.5.3 |
| Build | `cd embeded && pio run -e release` | `cd firmware-idf && ./build-role.sh leaf esp32` |
| Status | **known good on hardware** | scaffold: builds for esp32 + esp32c3, **boots on the real node**, roles stubbed |

The fork is **additive and reversible**. No slice of the epic edits `embeded/`,
which stays buildable and flashable throughout, and the epic ends in an explicit
adopt-or-discard decision (TEC-NATKIT-27). If the fork is discarded, deleting
`firmware-idf/` is the whole cleanup.

### Which firmware is on which board

Last recorded state — **update this table when you flash something**, and note
that it is a record rather than a measurement (nothing is read back off a board):

| Board | Firmware | Recorded |
|---|---|---|
| natKit-IMU node — ESP32-PICO-V3-02 rev v3.0, MAC `0c:8b:95:96:b9:f4`, BNO08x | **`embeded/`**, rebuilt and re-uploaded from `firmware-idf-fork` (identical source to `trunk`; the fork adds no files to `embeded/`). Verified streaming. | 2026-08-10 |

That board briefly ran `firmware-idf/`'s leaf image on 2026-08-10 to prove the
fork boots, and was restored with the rollback command below. A pre-flash 4 MB
dump of the working firmware is kept at
`~/natkit-verification/598a800/embeded-preflash-backup.bin` (sha256 in
`backup.sha256`) if a byte-exact restore is ever wanted:
`esptool write_flash 0 embeded-preflash-backup.bin`.

### Putting the current firmware back

One command, from a checkout of this repo:

```bash
cd embeded && pio run -e release -t upload
```

That is the rollback. It rebuilds and flashes the Arduino firmware from whatever
commit is checked out, so `git -C . checkout trunk` first if the working tree has
moved on; add `--upload-port /dev/ttyUSB0` if more than one board is attached.
Nothing needs to be uninstalled or undone on the ESP-IDF side, because the two
trees share no toolchain, no build directory and no configuration.

> **Trap:** `espressif32` and `pioarduino` share
> `~/.platformio/packages/framework-arduinoespressif32` and cannot coexist. If
> the official platform has been installed since, remove that directory before
> the rollback build. (The IDF fork sidesteps this entirely — it never touches
> `~/.platformio` — but `embeded/` still lives under PlatformIO.)

## Hardware Requirements

- ESP32 development board (tested with Pico32)
- BNO08x IMU sensor (connected via I2C/SPI)
- USB cable for programming and power

## Software Requirements

- [PlatformIO](https://platformio.org/) (VS Code extension recommended)
- natKit backend running (see main repository README)

## Project Structure

```
natKit-IMU/
├── embeded/
│   ├── include/          # Header files
│   │   ├── DevConfig.hpp.example  # Configuration template
│   │   ├── ConnectionConfig.hpp   # Network configuration struct
│   │   ├── ImuReader.hpp          # IMU sensor interface
│   │   ├── ImuData.hpp            # IMU data structures
│   │   └── ...
│   ├── src/
│   │   └── main.cpp      # Main application code
│   └── platformio.ini    # PlatformIO configuration
├── firmware-idf/         # the native ESP-IDF fork (see its own README)
└── board/                # Hardware design files
```

Everything below this line describes `embeded/`, the PlatformIO firmware.

## Setup

### 1. Configure Development Settings

Copy the example configuration file and update with your local settings:

```bash
cd embeded/include
cp DevConfig.hpp.example DevConfig.hpp
```

Edit `DevConfig.hpp` with your WiFi credentials and natKit server address:

```cpp
#define DEV_WIFI_SSID "your_wifi_ssid"
#define DEV_WIFI_PASSWORD "your_wifi_password"
#define DEV_NATKIT_SERVER_ADDRESS "192.168.1.100"  // IP of machine running docker-compose
#define DEV_NATKIT_SERVER_PORT "38082"
```

**Note:** `DevConfig.hpp` is gitignored and should never be committed.

### 2. Build and Flash

Open the project in VS Code with PlatformIO extension, then:

```bash
# Build for release
pio run -e release

# Build for debug (with serial output)
pio run -e debug_verbose

# Upload to device
pio run -e release -t upload

# Monitor serial output
pio device monitor
```

## Build Environments

| Environment | Description |
|-------------|-------------|
| `release` | Optimized build, minimal serial output |
| `debug` | Release build with serial disabled |
| `debug_verbose` | Debug build with full serial output |

## Operation Modes

### Development Mode (Default)

When `ENABLE_CAPTIVE_PORTAL` is not defined, the device uses credentials from `DevConfig.hpp` and connects automatically.

### Captive Portal Mode

When built with `ENABLE_CAPTIVE_PORTAL` defined, the device:
1. Creates a WiFi access point named `natKit-ESP32-<unique_id>`
2. Serves a configuration web page at `http://4.3.2.1`
3. Allows users to enter WiFi credentials and server address via browser

## Data Flow

1. IMU sensor data is read at configured intervals
2. Data is timestamped using NTP-synchronized time
3. Data is buffered and sent in bulk via MQTT to the natKit broker
4. The natKit bridge forwards MQTT messages to Kafka for storage/processing

## Configuration Constants

Key configuration values are defined at the top of `main.cpp`:

| Constant | Default | Description |
|----------|---------|-------------|
| `MQTT_BUFFER_SIZE` | 16384 | MQTT message buffer size |
| `MQTT_MAX_CONNECT_RETRIES` | 30 | Max MQTT connection attempts |
| `SEND_MESSAGE_TASK_STACK_SIZE` | 18432 | FreeRTOS task stack size |

## Troubleshooting

### Device won't connect to WiFi
- Verify WiFi credentials in `DevConfig.hpp`
- Ensure the WiFi network is 2.4GHz (ESP32 doesn't support 5GHz)
- Check serial output in `debug_verbose` mode

### MQTT connection fails
- Verify the natKit server is running (`docker-compose up -d`)
- Check that the server IP address is correct and reachable
- Ensure port 1883 is accessible

### IMU not detected
- Check I2C/SPI connections
- Verify BNO08x sensor is properly powered
- Check serial output for sensor initialization errors

## Pin Configuration

Default pin assignments (may vary by board):

| Function | Pin |
|----------|-----|
| Status LED | 2 |
| Debug Pin 1 | 12 |
| Debug Pin 2 | 13 |
| Debug Pin 3 | 27 |

## License

See main natKit repository for license information.
