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

✅ **THE DECISION IS MADE: the fork was ADOPTED on 2026-08-17** (TEC-NATKIT-27), on
a head-to-head bench of both firmwares on the same two boards, the same broker and
the same three hours. Per node, two nodes: **98 fresh samples/s against 56–80, in
half the bandwidth, with 1.4 ms of inter-arrival jitter against 17 ms and
node-to-node clock agreement of 0.1–0.2 ms against 2.7–5.5** — plus the magnetometer,
which `embeded/` cannot carry.

`embeded/` is **kept as a rollback path, not retired**, and reviewed on
**2026-09-15**. For it to stay a real rollback rather than a tree that merely
compiles, its libnatkit-core pin has to be bumped when the core moves, and **it has
to be flashed onto a board and measured once per review cycle** — 2026-08-17 was the
first time in months, and it immediately turned out to be incapable of running two
nodes at once (see below).

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

| Board | Port (by-id is the stable name) | Firmware | Recorded |
|---|---|---|---|
| **ESP32-D0WD-V3 rev 3.1**, MAC `30:c9:22:33:0c:ec`, id 53640420330732 | CP2102, serial `0001` | `firmware-idf/` **primary** — the WiFi rig's hub. ESP-NOW + serial uplink, no network of its own. | 2026-08-25 |
| **ESP32-C3 (QFN32) rev v0.4**, MAC `dc:da:0c:d1:49:38`, id 242829076023608 | CP2102**N**, serial `f46f1cbd859ded11a31c5f84e259fb3e` | `firmware-idf/` **gateway** — serial in, WiFi + MQTT out. ⚠️ The only board holding credentials (`main/DevConfig.hpp`). | 2026-08-25 |
| ESP32-S3 (ESP Thread Border Router + W5500 Ethernet), MAC `b8:f8:62:62:f7:3c`, id 203376942053180 | Espressif native USB, serial = its MAC | `firmware-idf/` **primary** (Ethernet uplink). ⚠️ **Powered down 2026-08-25** while the WiFi rig runs — two hubs on one channel adopt each other. ⚠️ Opening its USB console RESETS it *and re-enumerates*, so `capture.py` returns an empty file — diagnose it from the published status. | 2026-08-25 |
| ESP32-PICO-V3-02, MAC `0c:8b:95:96:bc:4c`, id 13793649671244, BNO08x | CH340, serial `5185026888` | `firmware-idf/` **leaf** | 2026-08-17 |
| ESP32, MAC `4c:75:25:a4:45:3c`, id 84066026407228, BNO08x | CH340, serial `5185027828` | `firmware-idf/` **leaf**. ⚠️ Was suspected bad; it is not — see below. | 2026-08-25 |
| ESP32, MAC `0c:8b:95:96:b9:f4`, id 13793649670644 — **the board previously believed damaged** | CH340, serial `5185027171` or `5185027831`, ⚠️ **not determined which** | `firmware-idf/` **leaf** | 2026-08-18 |
| ESP32, MAC `0c:8b:95:94:ef:d0`, id 13793649553360 | CH340, the other of `5185027171` / `5185027831` | `firmware-idf/` **leaf** | 2026-08-18 |
| **ESP32-PICO-V3-02 rev 3.0**, MAC `0c:8b:95:94:f0:78`, id 13793649553528 | CH340, serial `5185027088` | added to the bench 2026-08-25; streamed briefly, **not currently on the radio** | 2026-08-25 |
| **ESP32 (unknown revision)** | CH340, serial `5185027373` | ⚠️ **DEAD — no 3.3 V rail.** Bridge enumerates, chip never answers on any baud, and the onboard LEDs do not light. Not a cable and not the auto-reset circuit. | 2026-08-25 |

⚠️ **`5185027828` WAS WRONGLY SUSPECTED, and the evidence against it was confounded.**
On 2026-08-25 it showed a `NatKitNodeStatusV1` topic with no `Data` topic, which looks
exactly like a node that is registered and delivering nothing. It was simply
**unplugged** — the hub keeps publishing a registry entry for a leaf that is gone
(TEC-NATKIT-81). Reconnected, it delivers 10.0 frames/s with **zero sequence gaps**
and the strongest RSSI of the four (−43 dBm mean, −57 worst). A missing `Data` topic
is not evidence about a board until you have checked the board is powered.

### The two-board WiFi rig

For a bench with no Ethernet port. The hub does ESP-NOW only and reaches the broker
through a second chip over a wire, which keeps the uplink **off the radio** — the
thing TEC-NATKIT-30 measured as costing ~85% of ESP-NOW frames when one chip tried
to do both.

```sh
# hub: ESP-NOW + serial uplink, no network of its own
./build-role.sh primary esp32 \
  -p /dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0 flash

# gateway: serial in, WiFi + MQTT out. Needs main/DevConfig.hpp (gitignored).
./build-role.sh gateway esp32c3 \
  -p /dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_f46f1cbd859ded11a31c5f84e259fb3e-if00-port0 flash
```

**Three wires, and it is a CROSSOVER:**

| From | To | Carries |
|---|---|---|
| ESP32 primary **GPIO 26** (UART1 TX) | C3 gateway **GPIO 6** (UART1 RX) | data and status, upward |
| C3 gateway **GPIO 5** (UART1 TX) | ESP32 primary **GPIO 25** (UART1 RX) | device commands, downward |
| GND | GND | mandatory shared reference |

⚠️ **TX to RX.** TX→TX puts two push-pull outputs on one net, and the symptom —
garbage the CRC silently discards — looks identical to a baud mismatch. ⚠️ **GND
only**; do not tie 3V3 between two USB-powered boards.

⚠️ **Diagnosing a dead link: `0 bytes` and `0 frames` are different faults.** A baud
mismatch, a floating line or a missing ground all produce *edges*, which land in the
gateway's `bytes_skipped` as resync garbage. Exactly zero **bytes** read means the RX
pin saw no transitions at all — an open circuit or the wrong pin, and nothing else.
So read `bytes_read` before suspecting the baud rate. That is what found a jumper on
the wrong pin on 2026-08-25 while the primary was reporting 45,662 bytes sent.

⚠️ **The uplink pins are per-target and a missing default is fatal**: classic ESP32
26/25, ESP32-C3 5/6, ESP32-S3 9/10. A target absent from that list inherits 26/25,
`uart_set_pin` rejects a GPIO the chip does not have, and the board **reboots in a
loop before there is any console output to say why**. The S3 pair is reasoned rather
than measured — that board runs Ethernet and has never called `uart_set_pin`.


⚠️ **PORT NUMBERS MOVE WHEN BOARDS ARE SWAPPED, AND THE PRIMARY IS NOT ALWAYS
ttyACM2.** Two flashes were aimed at the wrong board before this was noticed; esptool
refused them ("This chip is ESP32, not ESP32-S3") rather than bricking a leaf, which
is the only reason it was cheap. Identify a port before flashing it, without touching
the board:

```sh
udevadm info -q property -n /dev/ttyACM0 | grep -E 'ID_MODEL=|ID_SERIAL_SHORT='
ls -l /dev/serial/by-id/          # the stable names, immune to ttyACM renumbering
```

⚠️⚠️ **FLASH BY `/dev/serial/by-id/...`, NOT BY ttyACM NUMBER.** `-p /dev/ttyACM1`
aims at a *port*, not a *board*. esptool refuses a chip-type mismatch, so aiming a
leaf image at the S3 fails loudly — **but two ESP32 leaves can be flashed in the
wrong order with no error whatsoever**, which silently invalidates any A/B between
them. The by-id path names the board:

```sh
./build-role.sh leaf esp32 -p /dev/serial/by-id/usb-1a86_USB_Single_Serial_5185026888-if00 flash
```

(Measured 2026-08-17/18: the ttyACM numbers did NOT in fact move across a full day of
resets, reflashes and bootloader parks, nor when two more boards were plugged in — a
DTR/RTS reset does not re-enumerate a CH340. Use by-id anyway; the cost is zero and
the failure is silent.)

The S3 primary reports `Espressif / USB_JTAG_serial_debug_unit` and **its MAC as the
USB serial number**, so it is unambiguous. The ESP32 leaves report a `1a86` CH340
bridge with an opaque serial — but the serials are **distinct and stable**, so the
two leaves can be told apart without flashing anything after all. Mapped on
2026-08-17 by flashing one and watching which device id changed firmware:

| `ID_SERIAL_SHORT` | board | device id |
|---|---|---|
| `5185026888` | ESP32-PICO-V3-02, `0c:8b:95:96:bc:4c` | 13793649671244 |
| `5185027828` | ESP32, `4c:75:25:a4:45:3c` | 84066026407228 |

⚠️ **A LEAF PUBLISHES AGAIN LONG BEFORE IT IS AT FULL RATE.** Measured 2026-08-17:
first frame at **18.7 s**, still 174 sequence gaps in a 420 s window seven minutes
later, and one leaf still at 6.7% loss **40 minutes** after its reset. "It is
publishing again" is not "it has recovered", and a rate measured in between reads as
a fault that is not there. `embeded/` by contrast was back at full rate in ~8 s.

⚠️ **When a leaf is not delivering, read `beacons_missed` and `leaf_send_failures`
from its `NatKitNodeStatusV1`** — the bad node was missing 616 of 913 beacons with
1800 send failures, while the healthy one missed 22 and failed 4. The clock-fit
figures say nothing about it: both nodes had a fully saturated `residual_rms_ns` and
one of them was delivering perfectly.

⚠️⚠️ **BOARD `0c:8b:95:96:b9:f4` IS BACK, AND IT IS NOT OBVIOUSLY BROKEN.** It was
removed on 2026-08-14 as "physically banged up" after delivering 0-22 samples/s with
274 sequence gaps against another leaf's clean 10/s, and TEC-NATKIT-46 was closed on
that. Reconnected and reflashed on 2026-08-18 it streams at ~10 frames/s and **the hub
hears it at −36 dBm, the strongest of all four nodes.**

Treat the original diagnosis as unsafe rather than wrong: the evidence that condemned
it — one leaf delivering badly while its neighbour is clean, on identical firmware and
power — is exactly what the two undamaged boards have since been doing to each other,
alternating every 10-30 minutes with nothing touched (TEC-NATKIT-50). Physical damage
was a plausible story for a symptom that turns out not to need one.

⚠️ And do not read the reverse from one good hour either: a board looking healthy for
twenty minutes is precisely what that fault does to every board in turn.
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
