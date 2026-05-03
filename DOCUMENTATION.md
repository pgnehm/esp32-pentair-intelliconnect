# ESP32 Pentair Pool Controller — Complete Project Documentation

**Revision:** 2026-05-03
**Board:** ESP32-S3 Lonely Binary Gold Edition
**Firmware:** PlatformIO / Arduino Framework

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Hardware Requirements & Bill of Materials](#2-hardware-requirements--bill-of-materials)
3. [Wiring Instructions](#3-wiring-instructions)
4. [Software Setup](#4-software-setup)
5. [Configuration (config.h Walkthrough)](#5-configuration-configh-walkthrough)
6. [Architecture & Code Structure](#6-architecture--code-structure)
7. [Pentair Protocol Reference](#7-pentair-protocol-reference)
8. [MQTT Topics Reference](#8-mqtt-topics-reference)
9. [Home Assistant Integration](#9-home-assistant-integration)
10. [OTA Updates](#10-ota-updates)
11. [UDP Debug Logging](#11-udp-debug-logging)
12. [Known Limitations](#12-known-limitations)
13. [Troubleshooting Guide](#13-troubleshooting-guide)

---

## 1. Project Overview

This project turns an ESP32-S3 microcontroller into a Wi-Fi bridge for a Pentair pool equipment installation. The ESP32 connects to the pool's RS485 data bus, passively reads all equipment traffic, and publishes live state to a Home Assistant (HA) instance via MQTT. It can also send commands to the IntelliFlo VS pump and IntelliChlor salt chlorinator.

### What It Does

- Monitors pump RPM, wattage, and speed preset in real time
- Reads water temperature from the IntelliChlor cell's built-in thermistor
- Reads salt PPM, chlorinator output percentage, and warning flags
- Reports pool on/off state (inferred from pump RPM)
- Reports RS485 bus health and pump-packet health as binary connectivity sensors
- Accepts pump speed commands (Stop / Low / Normal / High / Max) from HA
- Accepts chlorinator output percentage commands (0–100%) from HA
- Exposes a remote restart button in HA
- Auto-discovers all entities in HA with no manual YAML configuration
- Broadcasts diagnostic logs over UDP for cable-free debugging
- Supports over-the-air (OTA) firmware updates

### What It Does NOT Do

- Control pool circuits (lights, cleaner) — the IntelliConnect controller ignores RS485 SET commands from external devices
- Control the gas heater setpoint or mode — same reason
- Read air temperature — not broadcast on RS485 by IntelliConnect
- Read heater setpoint — Action 0x08 packets are not present on this bus

### Equipment on This Installation

| Device | Protocol | RS485 Address |
|---|---|---|
| IntelliConnect controller | A5 | 0x10 |
| IntelliFlo VS pump | A5 | 0x60 |
| IntelliChlor salt chlorinator | IntelliChlor | 0x50 |
| Gas heater | Relay (controlled by IntelliConnect) | N/A |
| This ESP32 device | A5 + IntelliChlor | 0x20 |

---

## 2. Hardware Requirements & Bill of Materials

### 2.1 Required Components

| Item | Notes |
|---|---|
| ESP32-S3 Lonely Binary Gold Edition | Any ESP32-S3 DevKitC-1 compatible board will work |
| MAX485 RS485 transceiver module | Any breakout board with DE, RE, DI, RO pins |
| 5V power supply | To power the MAX485 VCC; the ESP32-S3 runs on 3.3V internally |
| Breadboard or PCB | For making connections |
| Jumper wires | Male-to-male or as needed |
| USB-C cable | For initial flashing via USB |
| RJ45 or terminal block connector | To connect to pool equipment RS485 wiring |

### 2.2 Bill of Materials — Detail

**ESP32-S3 Board**
The Lonely Binary Gold Edition is an ESP32-S3 in a standard 38-pin DevKitC-1 footprint. Any ESP32-S3-DevKitC-1 board will work as a drop-in replacement. UART0 is used for USB CDC (Serial monitor), so this project uses UART1 (GPIO17/18) for RS485.

**MAX485 Transceiver**
The MAX485 is a half-duplex RS485 transceiver. It converts between the ESP32's 3.3V UART signals and the differential RS485 bus voltage levels. Key notes:

- VCC must be 5V (not 3.3V) for reliable RS485 bus drive strength.
- The GPIO logic levels from the ESP32 (3.3V) are sufficient to drive MAX485 inputs because the MAX485 input logic threshold is below 3.3V.
- DE (Driver Enable) and RE (Receiver Enable) pins on the MAX485 are tied together and driven from a single GPIO (GPIO16). HIGH = transmit mode, LOW = receive mode.

**Power Supply**
The MAX485 requires 5V. The ESP32-S3 USB pin (VBUS) supplies 5V when the board is connected to USB. For standalone operation, use a 5V wall adapter or splice 5V from a nearby 12V-to-5V regulator. The ESP32-S3 board's onboard LDO accepts 5V input and provides 3.3V to the chip.

---

## 3. Wiring Instructions

### 3.1 MAX485 to ESP32-S3 Connections

| MAX485 Pin | ESP32-S3 GPIO | Description |
|---|---|---|
| VCC | 5V (VBUS) | Power supply — must be 5V |
| GND | GND | Common ground |
| RO (Receiver Output) | GPIO18 | RS485 bus data received by ESP32 |
| DI (Driver Input) | GPIO17 | ESP32 UART TX to MAX485 |
| DE (Driver Enable) | GPIO16 | Direction control (tied to RE) |
| RE (Receiver Enable) | GPIO16 | Direction control (tied to DE) |
| A | Pool RS485 A (non-inverted) | Differential bus line |
| B | Pool RS485 B (inverted) | Differential bus line |

**Important:** DE and RE on the MAX485 must be wired together to the same GPIO (GPIO16). Do not connect them to separate pins.

### 3.2 RS485 Bus Connection

The Pentair RS485 bus uses a two-wire differential pair labeled A and B (sometimes called + and -). These wires run between the IntelliConnect controller and each piece of equipment (pump, chlorinator, etc.).

To connect the ESP32 to the bus:

1. Locate the RS485 wiring going to or from the IntelliConnect controller.
2. Identify the A (non-inverted, often labeled +) and B (inverted, often labeled -) wires.
3. Connect MAX485 pin A to the bus A wire.
4. Connect MAX485 pin B to the bus B wire.
5. The ESP32 is a passive listener and does not interfere with normal operation.

**Note on bus termination:** For short cable runs (under ~10 meters) within a typical residential pool equipment pad, termination resistors are generally not required.

### 3.3 ASCII Wiring Diagram

```
ESP32-S3 Board                MAX485 Module         RS485 Bus
+------------------+          +----------+
|                  |          |          |
|  GPIO16 (DE/RE) ---------> DE         |
|                  |     +--> RE         |           Pool
|  GPIO17 (TX)    ---------> DI         |           Equipment
|                  |          |          |     A ----+---- IntelliFlo pump
|  GPIO18 (RX)    <--------- RO         |           |     IntelliChlor
|                  |          |     A -------+       |     IntelliConnect
|  5V (VBUS)      ---------> VCC        |   B -------+
|  GND            ---------> GND   B -------+
|                  |          |          |
+------------------+          +----------+
```

### 3.4 Pool Equipment Pad Notes

- The RS485 wiring is typically a 2-conductor shielded cable routed between equipment.
- The IntelliConnect controller usually has an RS485 terminal block labeled A and B (or Data+ / Data-).
- The pump and chlorinator wiring runs in a daisy-chain topology, not a star.
- Do not disconnect existing wiring — tap in using a wire nut or terminal block splice.

---

## 4. Software Setup

### 4.1 Prerequisites

1. **PlatformIO IDE** — Install the PlatformIO extension for Visual Studio Code, or use the PlatformIO CLI. Download from: https://platformio.org/install
2. **Visual Studio Code** (recommended host IDE) — Download from: https://code.visualstudio.com
3. **Git** (optional) — For cloning the project repository

### 4.2 Project Setup

**Step 1: Open the project in VS Code**

Open the folder `esp32-pentair-controller` in VS Code. PlatformIO will detect the `platformio.ini` file and offer to install the required platform and libraries automatically.

**Step 2: Install dependencies**

PlatformIO will download and install the following libraries automatically on the first build:

- PubSubClient 2.8 (MQTT client)
- ArduinoJson 7.x (JSON serialization for HA discovery payloads)
- ArduinoOTA (over-the-air firmware updates)
- WiFi (bundled with the ESP32 Arduino core)

If PlatformIO does not install them automatically, run:

```
pio lib install
```

**Step 3: Edit config.h**

Open `include/config.h` and set your WiFi credentials, MQTT broker IP, and any other site-specific values. See Section 5 for a full walkthrough.

**Step 4: Build the firmware**

Click the PlatformIO checkmark icon in VS Code (or run):

```
pio run
```

**Step 5: Flash via USB (first time)**

Connect the ESP32-S3 via USB-C. Put the board in download mode if required (hold BOOT, press RESET, release BOOT). Then:

```
pio run --target upload
```

Or use the PlatformIO upload arrow button in VS Code.

**Step 6: Verify over Serial**

Open the PlatformIO Serial Monitor at 115200 baud. You should see:

```
========================================
  ESP32 Pentair Pool Controller
========================================

[RS485] Initialized: baud=9600, RX=18, TX=17, DE/RE=16
[WiFi] Connecting to YourSSID...
[WiFi] Connected! IP: 192.168.86.118
[OTA] Ready
[NTP] Time sync started
[MQTT] Attempting connection...
[MQTT] Connected!
[MQTT] Subscribed to command topics
[MQTT] Published HA discovery configs
[MAIN] Setup complete. Listening on RS485 bus...
```

### 4.3 platformio.ini Reference

The project's `platformio.ini` should contain the following board and library configuration:

```ini
[env:esp32s3]
platform  = espressif32
board     = esp32-s3-devkitc-1
framework = arduino

lib_deps =
    knolleary/PubSubClient @ ^2.8
    bblanchon/ArduinoJson @ ^7

upload_port   = 192.168.86.118
upload_protocol = espota
upload_flags = --auth=pentair-ota

monitor_speed = 115200
```

The `upload_port`, `upload_protocol`, and `upload_flags` settings enable OTA upload by default. Change `upload_port` to your device's IP address.

---

## 5. Configuration (config.h Walkthrough)

All site-specific settings live in `include/config.h`. This file is not safe to commit to a public repository because it contains credentials.

### 5.1 WiFi Settings

```cpp
#define WIFI_SSID         "YourNetworkName"
#define WIFI_PASSWORD     "YourPassword"
```

Set these to your home Wi-Fi network's SSID and password. The ESP32 will attempt to connect at startup and reconnect automatically if the connection drops.

### 5.2 MQTT Settings

```cpp
#define MQTT_SERVER       "192.168.86.117"
#define MQTT_PORT         1883
#define MQTT_USER         "mqtt_user"
#define MQTT_PASSWORD     "mqtt_password"
#define MQTT_CLIENT_ID    "pentair-controller"
#define MQTT_TOPIC_PREFIX "pentair"
```

- `MQTT_SERVER` — The static IP address of your Home Assistant host (where Mosquitto runs).
- `MQTT_PORT` — Default is 1883 (plaintext). For TLS, change to 8883 and update the TLS configuration in `mqtt_handler.cpp`.
- `MQTT_USER` / `MQTT_PASSWORD` — Create a dedicated MQTT user in Home Assistant > Settings > People > Users or in Mosquitto configuration.
- `MQTT_CLIENT_ID` — Must be unique on the broker. If you run multiple ESP32 units, give each a distinct ID.
- `MQTT_TOPIC_PREFIX` — All topics are published under this prefix. Change it if you have name conflicts with other MQTT devices.

### 5.3 RS485 Pin Settings

```cpp
#define RS485_RX_PIN      18
#define RS485_TX_PIN      17
#define RS485_DE_RE_PIN   16
#define RS485_UART_NUM    1
```

These match the hardware wiring described in Section 3. Change only if you use different GPIO pins. `RS485_UART_NUM` must be 1 because UART0 is reserved for USB CDC on the ESP32-S3.

### 5.4 Device Addresses

```cpp
#define ADDR_BROADCAST    0x0F
#define ADDR_INTELLITOUCH 0x10
#define ADDR_PUMP         0x60
#define ADDR_CHLORINATOR  0x50
#define ADDR_THIS_DEVICE  0x20
```

These are standard Pentair RS485 addresses that apply to most IntelliConnect, IntelliFlo, and IntelliChlor installations. You should not need to change them unless you have a non-standard setup or address conflicts.

### 5.5 Pump Speed Programs

```cpp
#define PUMP_PROG_STOP   0x00
#define PUMP_PROG_1      0x08
#define PUMP_PROG_2      0x10
#define PUMP_PROG_3      0x18
#define PUMP_PROG_4      0x20
```

These are register values written to the IntelliFlo's program select register (0x0321). They activate pre-programmed speeds stored in the pump's internal memory. The actual RPM depends on how the pump was programmed using the pump's keypad or the Pentair app. On this installation:

- Program 1 = ~1000 RPM (Low)
- Program 2 = ~1640 RPM (Normal / daily schedule 09:00–17:00)
- Program 3 = ~2315–2420 RPM (High)
- Program 4 = ~1600–3450 RPM (Max)

### 5.6 Timing Settings

```cpp
#define WIFI_CONNECT_TIMEOUT_MS   10000
#define MQTT_RECONNECT_INTERVAL   5000
#define RS485_TX_DELAY_US         100
#define RS485_TX_COMPLETE_WAIT_MS 10
```

- `WIFI_CONNECT_TIMEOUT_MS` — How long to wait for WiFi association before continuing setup (10 seconds). The device retries on every loop() pass after timeout.
- `MQTT_RECONNECT_INTERVAL` — Minimum milliseconds between broker reconnect attempts (5 seconds). Prevents hammering a temporarily unavailable broker.
- `RS485_TX_DELAY_US` — Microseconds to wait after asserting DE/RE HIGH before writing the first bit. Required for MAX485 driver output to settle.
- `RS485_TX_COMPLETE_WAIT_MS` — Milliseconds to hold DE/RE HIGH after `flush()` returns so the last bits propagate off the bus wire before switching back to receive mode.

### 5.7 NTP Time Settings

```cpp
#define NTP_SERVER            "pool.ntp.org"
#define NTP_UTC_OFFSET_SEC    (-7 * 3600)
#define NTP_DST_OFFSET_SEC    0
```

Time is only used for human-readable timestamps in heartbeat UDP log messages. It is not critical to pool control operation.

Common UTC offset values:

| Timezone | NTP_UTC_OFFSET_SEC |
|---|---|
| PDT (Pacific Daylight) | -7 * 3600 = -25200 |
| PST (Pacific Standard) | -8 * 3600 = -28800 |
| MDT (Mountain Daylight) | -6 * 3600 = -21600 |
| EDT (Eastern Daylight) | -4 * 3600 = -14400 |
| EST (Eastern Standard) | -5 * 3600 = -18000 |

### 5.8 OTA Settings

```cpp
#define OTA_HOSTNAME      "pentair-controller"
#define OTA_PASSWORD      "pentair-ota"
```

Change `OTA_PASSWORD` before deploying. The hostname is used for mDNS discovery. The OTA upload target IP (192.168.86.118) is configured as a static DHCP lease in your router and referenced in `platformio.ini`, not in config.h.

---

## 6. Architecture & Code Structure

### 6.1 File Layout

```
esp32-pentair-controller/
├── include/
│   ├── config.h             — All site-specific settings and constants
│   ├── pentair_protocol.h   — Protocol types, enums, structs, and function declarations
│   ├── rs485.h              — RS485 driver class declaration
│   ├── mqtt_handler.h       — MQTT client wrapper class declaration
│   └── udp_log.h            — UDP broadcast logger class declaration
├── src/
│   ├── main.cpp             — Arduino setup/loop, WiFi, OTA, MQTT command dispatch
│   ├── pentair_protocol.cpp — Protocol parsers, builders, and state processors
│   ├── rs485.cpp            — RS485 UART driver implementation
│   ├── mqtt_handler.cpp     — MQTT publish, discovery, and connection management
│   └── udp_log.cpp          — UDP broadcast logger implementation
└── platformio.ini           — PlatformIO build configuration
```

### 6.2 Component Responsibilities

**main.cpp**
The top-level application. Initializes all subsystems in `setup()`, then in `loop()`:
- Checks WiFi and reconnects if needed
- Calls `ArduinoOTA.handle()` for OTA readiness
- Calls `mqtt.loop()` and `rs485.loop()`
- Checks for completed A5 packets and chlorinator packets, dispatches to protocol processors
- Runs three periodic tasks: state publish (every 10 s), chlorinator poll (every 30 s), heartbeat log (every 5 s)

**pentair_protocol.cpp / .h**
Pure data processing — no I/O. Contains:
- `parsePacket()` — Streaming A5 packet scanner. Scans the byte buffer for a valid FF 00 FF A5 preamble and verifies the 16-bit checksum.
- `parseChlorPacket()` — Streaming IntelliChlor scanner. Locates [10 02 ... 10 03] frames and verifies the 8-bit checksum.
- `processPacket()` — Updates `PoolState` from A5 packets (pump status, controller status).
- `processChlorPacket()` — Updates `PoolState` from IntelliChlor packets (salt PPM, output %, water temperature).
- Builder functions (`buildPacket`, `buildPumpProgramCommand`, `buildChlorSetOutput`, etc.) — Assemble outbound command packets.

**rs485.cpp / .h**
Hardware driver for the half-duplex RS485 bus via UART1:
- `begin()` — Configures UART and sets DE/RE pin to receive mode.
- `loop()` — Drains the UART hardware FIFO into a ring buffer, strips unrecognized leading bytes, dispatches to the appropriate parser based on the first byte (0xFF → A5, 0x10 → IntelliChlor), and slides consumed bytes off the buffer.
- `sendPacket()` — Asserts DE/RE HIGH, writes bytes, waits for flush and propagation, then returns to receive mode.

**mqtt_handler.cpp / .h**
MQTT client wrapper built on PubSubClient:
- `begin()` — Configures server and callback.
- `loop()` — Manages reconnection with back-off; forwards to PubSubClient.
- `publishState()` — Publishes all `PoolState` fields as retained MQTT messages, rate-limited to once every 2 seconds.
- `publishDiscovery()` — Publishes Home Assistant auto-discovery JSON payloads for all entities. Runs on every (re)connect. Also publishes empty payloads to stale discovery topics from previous firmware versions to clean them up.

**udp_log.cpp / .h**
Thin wrapper around `WiFiUDP`. Mirrors all messages to both USB Serial and UDP broadcast (255.255.255.255 port 4210) so the device can be monitored wirelessly.

### 6.3 Data Flow

```
RS485 Bus
    |
    v
RS485::loop()         — raw bytes into ring buffer
    |
    v
parsePacket()         — A5 frames
parseChlorPacket()    — IntelliChlor frames
    |
    v
processPacket()       — updates PoolState
processChlorPacket()  — updates PoolState
    |
    v
MqttHandler::publishState()  — posts to MQTT broker
    |
    v
Home Assistant MQTT integration
    |
    v
User sees live pool state / sends commands
    |
    v
MQTT command arrives → handleMqttCommand() → RS485::sendPacket()
```

### 6.4 Pump Speed Change Sequence

Changing the pump speed requires three RS485 commands in sequence because the pump must first be placed under remote control before it will accept a program write:

1. **Remote control grant** (`ACTION_PUMP_REMOTE`, data = 0xFF) — tells the pump to accept external commands instead of following its internal schedule.
2. **Program write** (`ACTION_PUMP_WRITE`, register 0x0321, value = program number) — selects the speed program.
3. **Remote control release** (`ACTION_PUMP_REMOTE`, data = 0x00) — returns the pump to local/schedule control.

A 100 ms delay between each step gives the pump time to process each command. Skipping the release step leaves the pump in remote control mode, which means the IntelliConnect schedule will not be able to change the speed until the pump is power-cycled.

---

## 7. Pentair Protocol Reference

### 7.1 A5 Protocol (Controller / Pump / Heater)

The A5 protocol is used by the IntelliConnect controller, IntelliFlo pump, and gas heater. It is a byte-framed serial protocol at 9600 baud 8N1.

**Frame Structure:**

```
[FF] [00] [FF] [A5] [proto] [dst] [src] [action] [len] [data × len bytes] [ckH] [ckL]
 ^-- 3-byte preamble --^    ^---------- header (A5 starts checksum) -----------^  ^--checksum--^
```

| Field | Size | Description |
|---|---|---|
| Preamble | 3 bytes | Always FF 00 FF |
| A5 marker | 1 byte | Always 0xA5; checksum coverage starts here |
| Protocol | 1 byte | Always 0x00 on IntelliConnect bus |
| Dst | 1 byte | Destination device address |
| Src | 1 byte | Source device address |
| Action | 1 byte | Command / response code (see table below) |
| Len | 1 byte | Number of data bytes to follow |
| Data | 0–64 bytes | Payload (Len bytes) |
| ChecksumH | 1 byte | High byte of 16-bit checksum |
| ChecksumL | 1 byte | Low byte of 16-bit checksum |

**Checksum Calculation:**
16-bit sum of all bytes from the A5 marker through the last data byte (inclusive). Does not include the preamble bytes or the checksum bytes themselves.

**A5 Action Codes:**

| Code | Name | Direction | Description |
|---|---|---|---|
| 0x01 | ACTION_PUMP_WRITE | Controller → Pump | Write a register value to the pump |
| 0x02 | ACTION_STATUS_RESPONSE | Controller → Broadcast | Equipment status (circuits, temps, heat) |
| 0x04 | ACTION_PUMP_REMOTE | ESP32 → Pump | Grant (0xFF) or release (0x00) remote control |
| 0x05 | ACTION_PUMP_MODE | Controller → Pump | Set pump operating mode |
| 0x06 | ACTION_PUMP_RUN | Controller → Pump | Set pump run/stop state |
| 0x07 | ACTION_PUMP_STATUS | Pump → Controller | Pump telemetry (RPM, watts) |
| 0x08 | ACTION_HEAT_STATUS | Controller → Broadcast | Temperature setpoints (not seen on this bus) |
| 0x86 | ACTION_SET_CIRCUIT | External → Controller | Turn circuit on/off (ignored by IntelliConnect) |
| 0x88 | ACTION_SET_HEAT_MODE | External → Controller | Set heat mode/setpoint (ignored by IntelliConnect) |
| 0xC8 | ACTION_STATUS_REQUEST | Any → Controller | Request a status response broadcast |

**ACTION_PUMP_STATUS (0x07) Data Layout:**

| Byte | Content |
|---|---|
| [3:4] | Pump watts (big-endian 16-bit) |
| [5:6] | Pump RPM (big-endian 16-bit) |

**ACTION_STATUS_RESPONSE (0x02) Data Layout:**

| Byte | Content |
|---|---|
| [2:3] | Circuit bitmask (16-bit). Bit N-1 = circuit N state. |
| [14] | Water temperature °F (unreliable on this bus; use IntelliChlor instead) |
| [16] | Heater state: 0x00 = off, 0x20 = actively firing |
| [18] | Air temperature °F |
| [22] | Heat mode (lower 2 bits = pool: 0=off, 1=heater, 2=solar, 3=solar-only) |

### 7.2 IntelliChlor Protocol (Salt Chlorinator)

The IntelliChlor uses a completely separate framing scheme on the same physical RS485 bus.

**Frame Structure:**

```
[10] [02] [dst] [action] [payload...] [checksum] [10] [03]
 ^-start-^                                        ^--end--^
```

| Field | Size | Description |
|---|---|---|
| Start marker | 2 bytes | Always 10 02 |
| Dst | 1 byte | Destination address (0x50 = chlorinator) |
| Action | 1 byte | Command/response code |
| Payload | variable | Action-specific data |
| Checksum | 1 byte | (18 + sum of dst + action + payload bytes) & 0xFF |
| End marker | 2 bytes | Always 10 03 |

**Checksum Calculation:**
Add 18 (decimal) to the sum of all bytes between the start and end markers (dst + action + payload). Take the result modulo 256. The constant 18 (0x12) is part of the IntelliChlor specification.

**IntelliChlor Action Codes:**

| Code | Name | Description |
|---|---|---|
| 0x00 | CHLOR_GET_STATUS | Query: request current chlorinator status |
| 0x01 | CHLOR_STATUS_RESP | Response: [output%][salt_raw][flags] |
| 0x11 | CHLOR_SET_OUTPUT | Command: set output percentage (0–100) |
| 0x12 | CHLOR_OUTPUT_RESP | Acknowledgement to SET_OUTPUT: same data layout as STATUS_RESP |
| 0x14 | CHLOR_GET_VERSION | Query: request firmware version |
| 0x03 | CHLOR_VERSION_RESP | Response: firmware version string |
| 0x16 | CHLOR_EXTENDED_STATUS | Extended status: byte[0]=water temp °F (most reliable source) |

**CHLOR_STATUS_RESP (0x01) and CHLOR_OUTPUT_RESP (0x12) Data Layout:**

| Byte | Content |
|---|---|
| [0] | Chlorine output percentage (0–100) |
| [1] | Salt PPM raw value (multiply by 50 to get PPM) |
| [2] | Status flags: bit 2 (0x04) = low salt warning; bit 1 (0x02) = inspect cell |

**CHLOR_EXTENDED_STATUS (0x16) Data Layout:**

| Byte | Content |
|---|---|
| [0] | Water temperature in degrees Fahrenheit |
| [1] | Output percentage |
| [2:3] | Additional status flags |

**Salt PPM Calculation:**
`salt_ppm = raw_byte × 50`
Example: raw value 0x3C (60 decimal) → 60 × 50 = 3000 PPM.

### 7.3 Device Addresses

| Address | Device |
|---|---|
| 0x0F | Broadcast (A5) |
| 0x10 | IntelliConnect controller / IntelliTouch |
| 0x20 | Wireless remote / external device (this ESP32) |
| 0x50 | IntelliChlor salt chlorinator |
| 0x60 | IntelliFlo VS pump (first pump) |

---

## 8. MQTT Topics Reference

All topics use the prefix `pentair` (configurable via `MQTT_TOPIC_PREFIX` in config.h). All state topics are published with the MQTT retain flag so Home Assistant always shows the last known value after a restart or reconnect.

### 8.1 State Topics (Read-Only)

| Topic | Payload | Notes |
|---|---|---|
| pentair/pool/state | ON / OFF | Inferred from pump RPM > 0 |
| pentair/water_temp | Integer (°F) | From IntelliChlor cell thermistor |
| pentair/pump/state | ON / OFF | Pump running state |
| pentair/pump/rpm | Integer | Live pump RPM |
| pentair/pump/watts | Integer | Live pump power consumption |
| pentair/pump/speed/state | Stop / Low / Normal / High / Max | Nearest speed preset derived from RPM |
| pentair/chlorinator/output | Integer (0–100) | Chlorinator output percentage |
| pentair/chlorinator/salt_ppm | Integer | Salt level in PPM |
| pentair/chlorinator/state | ON / OFF | Chlorinator actively producing chlorine |
| pentair/chlorinator/low_salt | ON / OFF | Low salt warning flag |
| pentair/chlorinator/check_cell | ON / OFF | Inspect cell warning flag |
| pentair/rs485_alive | ON / OFF | ON if RS485 bus activity within last 60 s |
| pentair/pentair_alive | ON / OFF | ON if A5 pump packets within last 60 s |

### 8.2 Command Topics (Writable)

| Topic | Accepted Payloads | Effect |
|---|---|---|
| pentair/pump/speed/set | Stop, Low, Normal, High, Max | Changes pump speed via 3-step RS485 sequence |
| pentair/chlorinator/set | 0–100 (integer string) | Sets chlorinator output percentage |
| pentair/restart | PRESS | Triggers ESP.restart() on the ESP32 |

### 8.3 Pump Speed Mapping

| MQTT Payload | Pump Program | Approximate RPM (this system) |
|---|---|---|
| Stop | PUMP_PROG_STOP | 0 |
| Low | PUMP_PROG_1 | ~1000 |
| Normal | PUMP_PROG_2 | ~1640 |
| High | PUMP_PROG_3 | ~2315–2420 |
| Max | PUMP_PROG_4 | ~1600–3450 |

### 8.4 Health Sensor Behavior

The `rs485_alive` and `pentair_alive` topics carry ON/OFF values:

- **ON** — The relevant activity was seen within the last 60 seconds.
- **OFF** — No activity for more than 60 seconds (bus is silent or device is offline).
- **Unavailable** — The ESP32 itself has not published for more than 30 seconds. This state is displayed by Home Assistant automatically because the binary sensor discovery config includes `expire_after: 30`.

---

## 9. Home Assistant Integration

### 9.1 MQTT Integration Setup

1. In Home Assistant, go to **Settings → Devices & Services → Add Integration**.
2. Search for and add **MQTT**.
3. Enter your broker details:
   - Host: 192.168.86.117 (your HA host IP, or 127.0.0.1 if Mosquitto runs on the same host)
   - Port: 1883
   - Username: mqtt_user
   - Password: (your MQTT password)
4. Click Submit. HA will connect to the broker.

### 9.2 Auto-Discovery

When the ESP32 firmware connects to the MQTT broker, it automatically publishes Home Assistant MQTT auto-discovery configuration payloads. No manual YAML configuration is required.

The following entities will appear in HA under a single device called **"Pentair Pool Controller"** (manufacturer: Pentair, model: IntelliConnect):

**Sensors:**
- Pool (state: ON/OFF)
- Water Temp (°F, device class: temperature)
- Pump (state: ON/OFF)
- Pump RPM
- Pump Power (W, device class: power)
- Chlorinator Output (%)
- Salt Level (ppm)
- Chlorinator Active (ON/OFF)
- Salt Low (ON/OFF)
- Check Cell (ON/OFF)

**Select entity:**
- Pump Speed (options: Stop, Low, Normal, High, Max)

**Binary sensors (device class: connectivity):**
- RS485 Bus (Connected / Disconnected)
- Pentair Pump (Connected / Disconnected)

**Button:**
- Restart ESP32

### 9.3 Lovelace Dashboard Example

Add these cards to a Lovelace dashboard for a quick pool overview:

**Entities card for status:**

```yaml
type: entities
title: Pool Status
entities:
  - entity: sensor.pool
  - entity: sensor.water_temp
  - entity: sensor.pump
  - entity: sensor.pump_rpm
  - entity: sensor.pump_power
  - entity: sensor.chlorinator_output
  - entity: sensor.salt_level
```

**Select card for pump speed control:**

```yaml
type: entities
title: Pool Controls
entities:
  - entity: select.pump_speed
  - entity: sensor.chlorinator_active
  - entity: button.restart_esp32
```

**Connectivity status card:**

```yaml
type: entities
title: Connectivity
entities:
  - entity: binary_sensor.rs485_bus
  - entity: binary_sensor.pentair_pump
```

### 9.4 Automation Examples

**Alert when salt is low:**

```yaml
alias: Pool Salt Low Alert
trigger:
  - platform: state
    entity_id: binary_sensor.salt_low
    to: "on"
action:
  - service: notify.mobile_app
    data:
      message: "Pool salt level is low. Check chlorinator."
```

**Set pump to Low speed after hours:**

```yaml
alias: Pool Pump Night Mode
trigger:
  - platform: time
    at: "21:00:00"
action:
  - service: select.select_option
    target:
      entity_id: select.pump_speed
    data:
      option: Low
```

### 9.5 Notes on Chlorinator Control

The IntelliConnect controller runs its own control loop for the IntelliChlor on a roughly 30-second cycle in Auto mode. If you send a chlorinator output command from HA, the IntelliConnect may override it on the next cycle. Manual chlorinator output control is most useful when the IntelliConnect is in Manual mode or when you want a temporary override.

---

## 10. OTA Updates

### 10.1 First-Time Setup

OTA (Over-the-Air) updates are built into the firmware and enabled automatically. The ESP32 listens for OTA upload requests on its IP address.

Assign your ESP32 a static IP address via your router's DHCP reservation feature. The current OTA target IP for this installation is 192.168.86.118.

### 10.2 Uploading New Firmware

Once the device is running and on the network, flash new firmware wirelessly:

```
pio run --target upload
```

PlatformIO reads the `upload_port` from `platformio.ini` (192.168.86.118) and uses the ArduinoOTA protocol with the password from `OTA_PASSWORD` in config.h.

Alternatively, specify the port manually:

```
pio run --target upload --upload-port 192.168.86.118
```

### 10.3 OTA Progress

During an OTA update the ESP32 prints progress to both Serial and the UDP log:

```
[OTA] Starting update...
[OTA] Progress: 25%
[OTA] Progress: 50%
[OTA] Progress: 75%
[OTA] Done. Rebooting.
```

### 10.4 OTA Security

The OTA password (`pentair-ota`) should be changed to something private before deploying the device on a shared network. The password is set in config.h and compiled into the firmware. It is also referenced in `platformio.ini` via `--auth=pentair-ota`. Update both locations when changing the password.

---

## 11. UDP Debug Logging

### 11.1 Overview

The firmware broadcasts all diagnostic messages to the LAN broadcast address (255.255.255.255) on UDP port 4210. This allows monitoring the device without a USB cable — useful once the device is mounted in the pool equipment enclosure.

### 11.2 Receiving UDP Logs

**On Windows (PowerShell):**

```powershell
$udp = [System.Net.Sockets.UdpClient]::new(4210)
while ($true) {
    $ep = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
    $data = $udp.Receive([ref]$ep)
    [System.Text.Encoding]::UTF8.GetString($data)
}
```

**On Linux / macOS:**

```bash
nc -u -l 4210
```

**Python (cross-platform, one-liner):**

```python
import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('',4210)); [print(s.recvfrom(1024)[0].decode(),end='') for _ in iter(int,1)]
```

### 11.3 Log Message Reference

| Prefix | Meaning |
|---|---|
| [WiFi] | WiFi connection events |
| [MQTT] | MQTT connection, subscription, and publish events |
| [OTA] | OTA update progress |
| [NTP] | Time synchronization |
| [RS485] | UART initialization |
| [PARSE] | Successfully parsed A5 packet (src, dst, action, length) |
| [PKT] | A5 packet hex dump (first 16 data bytes) |
| [CHLOR] | IntelliChlor packet hex dump |
| [RAW] | Raw hex dump of bytes received from the RS485 bus (every 2 s) |
| [CMD] | Incoming MQTT command being processed |
| [ALIVE] | Periodic heartbeat: timestamp, uptime, IP, MQTT status, byte count |

### 11.4 Sample Log Output

```
[ALIVE] 2026-05-03 14:23:01 uptime=3600s ip=192.168.86.118 mqtt=OK rx_bytes=142880
[PKT] src=0x60 dst=0x10 action=0x07 len=15 | 00 00 00 00 5A 06 68 ...
[PARSE] src=60 dst=10 act=07 len=15
[CHLOR] action=0x16 len=4 | 59 46 00 00
[CMD] pentair/pump/speed/set = Normal
[CMD] Pump speed set to Normal
```

---

## 12. Known Limitations

### 12.1 Circuit Control Not Available

The IntelliConnect controller (address 0x10) does not accept A5 SET commands (action codes 0x86 and 0x88) from external RS485 devices. This means:

- Pool light, cleaner, and all other circuit on/off commands are ignored.
- Heater mode and setpoint commands are ignored.
- These limitations are a property of the IntelliConnect hardware, not a bug in this firmware.

Workaround: If circuit control is required, use the Pentair IntelliConnect app or install an IntelliTouch or EasyTouch controller, which does accept external RS485 SET commands.

### 12.2 Chlorinator Override by IntelliConnect

When the IntelliConnect is in Auto mode, it polls and commands the IntelliChlor on approximately a 30-second cycle. Any chlorinator output percentage set via this ESP32 may be overridden by the next IntelliConnect poll cycle.

### 12.3 Air Temperature Not Available

The IntelliConnect does not broadcast air temperature on the RS485 bus. While the A5 ACTION_STATUS_RESPONSE packet contains an air temperature byte, IntelliConnect does not populate it with valid data in this installation.

### 12.4 Water Temperature Source

Water temperature should be read from the IntelliChlor CHLOR_EXTENDED_STATUS packet (action 0x16), not from the controller's A5 status response. The A5 status response water temperature byte is unreliable on this installation. The chlorinator cell has its own thermistor and provides the authoritative reading.

### 12.5 Heater Setpoint Readback

Action 0x08 (ACTION_HEAT_STATUS) packets containing heater setpoints are not observed on this RS485 bus. The heater setpoint cannot be read back.

### 12.6 Single Packet Buffer

The RS485 driver holds only one parsed packet (A5 or chlorinator) at a time between loop() calls. If two complete packets arrive between loop() iterations, only the second one will be available via `getPacket()`. In practice, loop() runs at several hundred Hz and Pentair packets arrive at most a few times per second, so this is not a problem in normal operation.

---

## 13. Troubleshooting Guide

### 13.1 Device Does Not Connect to WiFi

**Symptom:** Serial shows `[WiFi] Connection timeout!` repeatedly.

**Checks:**
1. Verify `WIFI_SSID` and `WIFI_PASSWORD` in config.h are correct (case-sensitive).
2. Confirm the network is 2.4 GHz — the ESP32 does not support 5 GHz WiFi.
3. Check that the device is within range of the access point.
4. Try a 60-second power cycle of both the router and the ESP32.

### 13.2 MQTT Shows "Failed, rc=X"

**Common rc codes:**

| Code | Meaning | Resolution |
|---|---|---|
| -2 | Cannot reach broker | Check MQTT_SERVER IP and that port 1883 is open |
| -4 | Connection timeout | Router firewall blocking port 1883; check Mosquitto is running |
| 4 | Authentication failed | Verify MQTT_USER / MQTT_PASSWORD in config.h |
| 5 | Not authorized | Check Mosquitto ACL rules for the user |

### 13.3 No Pool Data in Home Assistant

**Step 1:** Check that the MQTT integration is connected in HA (Settings → Devices & Services → MQTT).

**Step 2:** Use the HA MQTT listener (Developer Tools → MQTT → Listen to a topic) and subscribe to `pentair/#`. You should see messages arriving every few seconds.

**Step 3:** Check the UDP log for `[PARSE]` messages. If packets are being parsed but not appearing in HA, the discovery payload may not have arrived. Power-cycle the ESP32 to re-send discovery.

**Step 4:** If no `[PARSE]` messages appear in the UDP log, the RS485 connection is not working. Check wiring (Section 3). Verify the `rs485_alive` binary sensor is ON.

### 13.4 Pump Speed Command Has No Effect

**Possible causes:**

1. The pump is in manual mode at the keypad — press a button on the pump keypad to clear manual mode, then retry.
2. The 3-step sequence is not completing — check the UDP log for `[CMD] Pump speed set to X`. If absent, the MQTT command topic subscription is not working (check MQTT_TOPIC_PREFIX).
3. RS485 wiring polarity is reversed (A and B swapped) — the device will receive data correctly but transmitted commands may be corrupted. Try swapping the A and B wires.

### 13.5 Water Temperature Shows 0 or Never Updates

Water temperature comes from the CHLOR_EXTENDED_STATUS (action 0x16) packet from the IntelliChlor. If this packet is not seen:

1. Verify the IntelliChlor is powered on and communicating with the IntelliConnect.
2. Check the UDP log for `[CHLOR] action=0x16` entries.
3. The firmware also queries the chlorinator directly every 30 seconds. If the chlorinator is offline, this query will go unanswered. Check the `check_cell` sensor — if it is ON, the cell may need servicing.

### 13.6 HA Shows "Unavailable" for Binary Sensors

The binary sensor discovery config includes `expire_after: 30` (seconds). If the ESP32 has not published any MQTT message within 30 seconds, HA marks the sensor as Unavailable.

This typically means the ESP32 is:
- Offline (crashed, no power)
- Disconnected from MQTT (check `[MQTT]` log messages)
- Disconnected from WiFi

Use the Restart ESP32 button in HA if it is visible, or power-cycle the device.

### 13.7 OTA Upload Fails

**Error: "No response from device"**
- Confirm the device is running and reachable by pinging 192.168.86.118.
- Confirm the `upload_port` in `platformio.ini` matches the device's actual IP.
- The device must be in the main loop (not in a blocking operation) to accept OTA. Check for `[ALIVE]` heartbeat messages in the UDP log.

**Error: "Authentication failed"**
- The `--auth` value in `platformio.ini` must match `OTA_PASSWORD` in config.h.

**Error: "Not enough space"**
- The compiled firmware is too large for the OTA partition. Reduce enabled features or choose a board profile with a larger OTA partition.

### 13.8 RS485 Bus Noise / Garbage Data

If the UDP log shows mostly `[RAW]` data with no `[PARSE]` events:

1. Check that A and B wires are not swapped at either end.
2. Confirm the ESP32 GND is connected to the MAX485 GND.
3. Confirm the MAX485 VCC is receiving 5V (not 3.3V).
4. Confirm the baud rate in `PENTAIR_BAUD_RATE` is 9600.
5. Make sure the cable from the MAX485 A/B pins to the RS485 bus is short and not running parallel to high-voltage wiring.

### 13.9 Salt PPM Reads Incorrectly

The salt PPM value is `raw_byte × 50`. If the displayed value seems offset or wrong:

1. Cross-check against a manual salt test kit or the IntelliConnect app display.
2. If the IntelliConnect app shows a different value, the chlorinator cell may need calibration (a Pentair service procedure).
3. If the value is 0, no CHLOR_STATUS_RESP or CHLOR_OUTPUT_RESP packets have been received yet. Wait 30–60 seconds for the first poll cycle.

---

*End of documentation.*
