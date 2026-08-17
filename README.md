# natKit-IMU

ESP32-based IMU sensor device for the natKit BCI toolkit. This device collects inertial measurement data and streams it to the natKit backend via MQTT.

## There are two firmwares — read this first

This repository holds **two** node firmwares. They speak to the same broker and
the same topic names, so knowing which one is on a board is not optional.

| | [`embeded/`](embeded) | [`firmware-idf/`](firmware-idf) |
|---|---|---|
| What | the ORIGINAL firmware, now a rollback path — every node a full WiFi/MQTT/NTP client | **what the rig actually runs** (EPIC TEC-NATKIT-20): leaf/primary/gateway roles, ESP-NOW between nodes, one networked hub |
| Framework | Arduino via pioarduino (arduino-esp32 3.3.11 / ESP-IDF 5.5.5) | native ESP-IDF (`idf.py`), v5.5.3 |
| Build | `cd embeded && pio run -e release` | `cd firmware-idf && ./build-role.sh leaf esp32` |
| Status | buildable, kept as the rollback path; **benched head-to-head on 2026-08-17 (TEC-NATKIT-27)** | **in use on every board**, streaming 100 samples/s per leaf |

The fork is still **additive and reversible** — `embeded/` stays buildable and
flashable, and the epic ends in an explicit adopt-or-discard decision
(TEC-NATKIT-27). But "additive" no longer means "unused": the ESP-IDF tree is what
every board on the bench is running, and has been since 2026-08-12.

**So new work goes in `firmware-idf/`. Do not edit `embeded/` unless the change
actually requires it** — a change there cannot be verified, because no board runs
it, and it does not even build against the sibling submodule (see the wire-format
warning below). If a fix would be needed after a rollback, file a ticket saying so
rather than porting it pre-emptively.

⚠️ **The reverse is not a preference but a hazard: fixes made to `embeded/` before
the migration were not all carried across.** The EXECUTION_COMMAND / LOGGING_LOG
channel (`embeded/include/CommandChannel.hpp`) has no counterpart in
`firmware-idf/` at all, and it was verified on hardware a week before the switch.
Nothing errors — the backend half still works, so a command is published and
simply goes unsubscribed. TEC-NATKIT-39.

⚠️ **`embeded/` COULD NEVER RUN TWO NODES AT ONCE UNTIL 2026-08-17.** All three of
its `connect()` calls passed the literal MQTT client id `"natKit-IMU"`, and a broker
must evict an existing session when a second client presents the same id — so two of
these nodes took turns kicking each other off ~700 times a minute and **lost 41% of
their frames**. Fixed (`natkitMqttClientId()`); if you are on a checkout older than
that, do not benchmark two of them. Measured on TEC-NATKIT-27.

⚠️ **THE TWO FIRMWARES ARE NO LONGER WIRE-COMPATIBLE BY DEFAULT.** `firmware-idf/`
emits IMU frame **version 2** (13 floats, 62-byte samples, 644-byte frames, with
the magnetometer). `embeded/` emits **version 1** (10 floats, 50-byte samples, 524
bytes) and will keep doing so until its `platformio.ini` libnatkit-core pin is
bumped past the version-2 commit — it pins a GitHub commit rather than the sibling
submodule, so editing `libnatkit/lib/libnatkit-core` does nothing for it. Decoders
read both, so a rollback still produces valid recordings; they simply have no
magnetic field in them.

### Which firmware is on which board

Last recorded state — **update this table when you flash something**, and note
that it is a record rather than a measurement (nothing is read back off a board):

| Board | Port | Firmware | Recorded |
|---|---|---|---|
| ESP32-S3 (ESP Thread Border Router + W5500 Ethernet), MAC `b8:f8:62:62:f7:3c` | `/dev/ttyACM0` | `firmware-idf/` **primary**. ⚠️ Opening its USB console RESETS it *and re-enumerates*, so `capture.py` returns an empty file — diagnose it from the published status. | 2026-08-14 |
| ESP32-PICO-V3-02, MAC `0c:8b:95:96:bc:4c`, BNO08x | `/dev/ttyACM1` (serial `5185026888`) | `firmware-idf/` **leaf**. Flashed to `embeded/` and back on 2026-08-17 for TEC-NATKIT-27. | 2026-08-17 |
| ESP32, MAC `4c:75:25:a4:45:3c`, BNO08x | `/dev/ttyACM2` (serial `5185027828`) | `firmware-idf/` **leaf**. Flashed to `embeded/` and back on 2026-08-17 for TEC-NATKIT-27. | 2026-08-17 |

⚠️ **PORT NUMBERS MOVE WHEN BOARDS ARE SWAPPED, AND THE PRIMARY IS NOT ALWAYS
ttyACM2.** Two flashes were aimed at the wrong board before this was noticed; esptool
refused them ("This chip is ESP32, not ESP32-S3") rather than bricking a leaf, which
is the only reason it was cheap. Identify a port before flashing it, without touching
the board:

```sh
udevadm info -q property -n /dev/ttyACM0 | grep -E 'ID_MODEL=|ID_SERIAL_SHORT='
```

The S3 primary reports `Espressif / USB_JTAG_serial_debug_unit` and **its MAC as the
USB serial number**, so it is unambiguous. The ESP32 leaves report a `1a86` CH340
bridge with an opaque serial — but the serials are **distinct and stable**, so the
two leaves can be told apart without flashing anything after all. Mapped on
2026-08-17 by flashing one and watching which device id changed firmware:

| `ID_SERIAL_SHORT` | board | device id |
|---|---|---|
| `5185026888` | ESP32-PICO-V3-02, `0c:8b:95:96:bc:4c` | 13793649671244 |
| `5185027828` | ESP32, `4c:75:25:a4:45:3c` | 84066026407228 |

⚠️ **A LEAF TAKES MINUTES, NOT SECONDS, TO COME BACK TO FULL RATE AFTER A RESET.**
Measured 2026-08-17: first frame at **18.7 s**, but still 174 sequence gaps in a
420 s window seven minutes later, and only clean at ~10 minutes. "It is publishing
again" is not "it has recovered", and a rate measured in between reads as a fault
that is not there. (Suspected cause: the clock-fit latch-up in TEC-NATKIT-47.)
`embeded/` by contrast was back at full rate ~8 s after a reset.

⚠️ Board `0c:8b:95:96:b9:f4` was REMOVED on 2026-08-14 — physically damaged, and it
had been delivering 0-22 samples/s with 274 sequence gaps against the other leaf's
clean 10/s. It is still in the primary's NVS registry, so the hub publishes a
`NatKitNodeStatusV1` for a node that no longer exists.
Both leaves are currently pinned to 8.5 dBm with the transmit-power sweep OFF.

A pre-flash 4 MB dump of the working `embeded/` firmware from board `…b9:f4` is
kept at `~/natkit-verification/598a800/embeded-preflash-backup.bin` (sha256 in
`backup.sha256`) if a byte-exact restore is ever wanted:
`esptool write_flash 0 embeded-preflash-backup.bin`.

### Rolling back to the Arduino firmware

⚠️ This is a ROLLBACK, not a restore of the status quo — the ESP-IDF firmware is
what is deployed. One command, from a checkout of this repo:

```bash
cd embeded && pio run -e release -t upload
```

It rebuilds and flashes the Arduino firmware from whatever
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
