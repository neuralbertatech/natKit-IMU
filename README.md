# natKit-IMU

ESP32-based IMU sensor device for the natKit BCI toolkit. This device collects inertial measurement data and streams it to the natKit backend via MQTT.

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
└── board/                # Hardware design files
```

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
