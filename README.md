# ESP32 Pentair IntelliConnect Controller

An ESP32-S3 firmware that bridges a **Pentair IntelliConnect** pool equipment installation to **Home Assistant** over MQTT. The ESP32 taps into the pool's RS485 data bus as a passive listener, parses all equipment traffic in real time, and publishes live state to Home Assistant via MQTT auto-discovery. It can also send pump speed and chlorinator output commands directly to the equipment.

<img width="226" height="427" alt="image" src="https://github.com/user-attachments/assets/8c9dc330-a91e-42b2-bda2-b546c4d1c8b9" />
<img width="230" height="111" alt="image" src="https://github.com/user-attachments/assets/3f87ea5e-3f09-4df5-8106-41a70d2fdf95" />

---

## Table of Contents

1. [What It Does](#1-what-it-does)
2. [Known Limitations](#2-known-limitations)
3. [Parts Needed](#3-parts-needed)
4. [Wiring & Connections](#4-wiring--connections)
5. [Software Setup](#5-software-setup)
6. [Configuration Reference (config.h)](#6-configuration-reference-configh)
7. [Code Walkthrough](#7-code-walkthrough)
8. [Pentair Protocol Reference](#8-pentair-protocol-reference)
9. [MQTT Topics Reference](#9-mqtt-topics-reference)
10. [Home Assistant Integration](#10-home-assistant-integration)
11. [OTA Firmware Updates](#11-ota-firmware-updates)
12. [UDP Debug Logging](#12-udp-debug-logging)
13. [Troubleshooting](#13-troubleshooting)

---

## 1. What It Does

### Monitoring (passive, always on)
- **Pump**: live RPM, wattage, running state, and nearest speed preset name
- **Water temperature**: from the IntelliChlor cell's built-in thermistor (the most reliable source on this bus)
- **Chlorinator**: output percentage, salt PPM, active/running state, low-salt warning, inspect-cell warning
- **Bus health**: binary connectivity sensors for RS485 bus activity and pump-packet activity

### Control (commands sent to equipment)
- **Pump speed**: Stop / Standard / Heat / Cleaner / Test — sent as `ACTION_PUMP_MODE` + `ACTION_PUMP_RUN`, exactly replicating what the IntelliConnect itself sends. No bus-takeover handshake needed.
- **Chlorinator output**: 0–100% via IntelliChlor `CHLOR_SET_OUTPUT` command
- **Remote restart**: trigger `ESP.restart()` from a Home Assistant button

### Home Assistant integration
- Full **MQTT auto-discovery** on every (re)connect — all entities appear in HA automatically under a single device card, no YAML required
- Stale discovery configs from older firmware versions are automatically purged on boot
- All state topics published with the **retain** flag so HA always shows the last known value after a restart

### Developer / diagnostics
- **UDP broadcast logging** — all diagnostic messages are broadcast to the LAN on port 4210, so the device can be monitored wirelessly without a USB cable
- **OTA firmware updates** — flash new firmware over Wi-Fi using PlatformIO

---

## 2. Known Limitations

### Circuit on/off control is not possible
The IntelliConnect controller (address `0x10`) silently ignores RS485 `ACTION_SET_CIRCUIT` (0x86) and `ACTION_SET_HEAT_MODE` (0x88) commands from external devices. This means:
- Pool light, cleaner circuit, and any other relay circuit **cannot** be toggled over RS485 from this device.
- Gas heater mode and setpoint **cannot** be changed over RS485.

This is a hardware limitation of the IntelliConnect. The IntelliTouch and EasyTouch controllers *do* accept these commands — this limitation is specific to IntelliConnect.

### Chlorinator output may be overridden by IntelliConnect
When IntelliConnect is in Auto mode it polls and re-commands the IntelliChlor on roughly a 30-second cycle. Any output percentage set via this ESP32 may be overridden on the next IntelliConnect cycle. Permanent chlorinator control requires putting the IntelliConnect into Manual mode.

### Pump speed changes only work while the pump is running
The IntelliConnect is the master scheduler. If its schedule says the pump should be off, sending a pump speed command from HA will be immediately overridden by the IntelliConnect. Speed changes work reliably when the pool circuit is already active under the IntelliConnect's schedule.

### Only one pump supported
The firmware addresses a single IntelliFlo VS pump at the standard address `0x60`. Installations with two pumps would require code changes.

### Air temperature not available
IntelliConnect does not broadcast air temperature on the RS485 bus in this configuration.

### Heater setpoint not readable
`ACTION_HEAT_STATUS` (0x08) packets are not present on this bus, so the heater setpoint cannot be read back.

### Salt PPM: zero readings suppressed
The IntelliChlor occasionally transmits `0` for salt PPM. The firmware suppresses these zero readings and retains the last valid value in HA. The raw zero values are still visible in the UDP debug stream.

---

## 3. Parts Needed

| Qty | Component | Notes |
|-----|-----------|-------|
| 1 | **ESP32-S3 board** | Any ESP32-S3-DevKitC-1 compatible board. Tested on the Lonely Binary Gold Edition (38-pin DevKitC-1 footprint). Must be ESP32-**S3** — the project uses UART1 for RS485 since UART0 is consumed by USB CDC. |
| 1 | **MAX485 RS485 transceiver module** | Any breakout board exposing DE, RE, DI, RO, VCC, GND, A, B. Half-duplex. |
| 1 | **5V power supply** | MAX485 VCC must be 5V. The ESP32-S3 board's onboard LDO accepts 5V on the VBUS pin and produces 3.3V internally. A USB phone charger works for bench testing. For permanent install, use a 5V wall adapter or tap a 12V→5V regulator from the equipment power. |
| 1 | **2-wire shielded cable** | To tap the RS485 bus. Short runs under ~10 m in a residential equipment pad do not need termination resistors. |
| — | **Jumper wires** | For connections to the MAX485 module. |
| 1 | **USB-C cable** | For the first USB flash. Subsequent updates are OTA. |
| — | **Wire nuts or terminal block** | For tapping the existing RS485 bus wiring without cutting it. |

### Why MAX485 and not another transceiver?
The MAX485 is simple, cheap, well-documented, and its input logic threshold is low enough to be reliably driven by the ESP32's 3.3V GPIO outputs even though the chip runs on 5V. No level-shifting is required on the data lines.

---

## 4. Wiring & Connections

### MAX485 to ESP32-S3

| MAX485 Pin | ESP32-S3 Pin | Function |
|------------|--------------|----------|
| VCC | 5V (VBUS) | Power — **must be 5V**, not 3.3V |
| GND | GND | Common ground |
| RO (Receiver Output) | GPIO 18 | RS485 data received → ESP32 UART RX |
| DI (Driver Input) | GPIO 17 | ESP32 UART TX → RS485 data transmitted |
| DE (Driver Enable) | GPIO 16 | Direction control — tie to RE |
| RE (Receiver Enable) | GPIO 16 | Direction control — tie to DE |
| A (non-inverted) | Pool RS485 A | Differential bus line + |
| B (inverted) | Pool RS485 B | Differential bus line − |

**Important:** Wire DE and RE together to a single GPIO (GPIO 16). Do not connect them to separate pins. When GPIO 16 is HIGH the MAX485 transmits; when LOW it receives.

### ASCII Wiring Diagram

```
  ESP32-S3                  MAX485 Module           RS485 Bus (to equipment)
 ┌────────────┐             ┌────────────┐
 │            │             │            │
 │  GPIO 16  ─┼────────────►DE          │
 │            │       ┌────►RE          │
 │  GPIO 17  ─┼───────┼───►DI          │
 │            │       │    │            │     A ──────┬── IntelliConnect
 │  GPIO 18  ◄┼───────┼────RO          │             ├── IntelliFlo pump
 │            │       │    │      A ───►┼─────┘       └── IntelliChlor
 │  5V (VBUS)─┼───────┼───►VCC         │
 │  GND      ─┼───────┴───►GND   B ───►┼──────────── (all equipment)
 └────────────┘             └────────────┘
```

### Connecting to the Pool Equipment Pad

The Pentair RS485 bus is a two-wire differential pair, typically labeled **A** (or +, Data+) and **B** (or −, Data−). It runs between the IntelliConnect controller and each piece of equipment in a **daisy-chain topology** (not a star).

1. Locate the RS485 terminal block on the IntelliConnect controller or the wiring between devices.
2. Identify the A and B wires. If unlabeled, try one orientation; if the ESP32 logs show garbage data, swap A and B.
3. Tap the wires using a wire nut or spare terminal on the IntelliConnect terminal block — **do not cut** the existing wiring.
4. The ESP32/MAX485 is a passive listener in receive mode and does not affect normal operation.

### Polarity Note
If A and B are swapped the device will still receive data (RS485 is differential and the MAX485 will invert the signals) but the parsed data will be garbage because Pentair uses a fixed polarity convention. Swap the A/B wires if you see no valid parsed packets in the UDP log.

---

## 5. Software Setup

### Prerequisites

| Tool | Purpose | Install |
|------|---------|---------|
| [Visual Studio Code](https://code.visualstudio.com) | IDE | Download from website |
| [PlatformIO extension](https://platformio.org/install/ide?install=vscode) | Build system | VS Code extension marketplace |
| Git | Clone the repo | [git-scm.com](https://git-scm.com) |

### Step 1 — Clone and open the project

```bash
git clone https://github.com/pgnehm/esp32-pentair-intelliconnect.git
cd esp32-pentair-intelliconnect
code .
```

PlatformIO will detect `platformio.ini` and offer to install the required platform and libraries automatically.

### Step 2 — Create your config.h

```bash
cp include/config.h.example include/config.h
```

Open `include/config.h` and fill in your values. At minimum you need:
- `WIFI_SSID` / `WIFI_PASSWORD`
- `MQTT_SERVER` — the LAN IP of your Home Assistant host
- `MQTT_USER` / `MQTT_PASSWORD`
- `OTA_PASSWORD` — change from the default before deploying

See [Section 6](#6-configuration-reference-configh) for a full walkthrough of every setting.

> **Security:** `config.h` is listed in `.gitignore` and must never be committed. The repo only contains `config.h.example` with placeholder values.

### Step 3 — Build the firmware

Click the PlatformIO checkmark (Build) button in VS Code, or run:

```bash
pio run
```

### Step 4 — Flash via USB (first time only)

Connect the ESP32-S3 via USB-C. Most boards enter download mode automatically; if not, hold **BOOT**, press **RESET**, release **BOOT**. Then:

```bash
pio run --target upload
```

Or click the PlatformIO right-arrow (Upload) button.

### Step 5 — Verify on Serial Monitor

Open the PlatformIO Serial Monitor at **115200 baud**. A successful startup looks like:

```
========================================
  ESP32 Pentair Pool Controller
========================================

[RS485] Initialized: baud=9600 RX=18 TX=17 DE/RE=16
[WiFi] Connecting to YourSSID...
[WiFi] Connected! IP: 192.168.x.x
[OTA] Ready
[NTP] Time sync started
[MQTT] Attempting connection...
[MQTT] Connected!
[MQTT] Subscribed to command topics
[MQTT] Published HA discovery configs
[MAIN] Setup complete. Listening on RS485 bus...
```

Shortly after, you should see `[PKT]` and `[CHLOR]` log lines as bus traffic is parsed.

### Step 6 — Assign a static IP (for OTA)

Set a static DHCP lease for the ESP32's MAC address in your router so its IP is predictable. Update `upload_port` in `platformio.ini` to match. Subsequent firmware updates can then be done wirelessly:

```bash
pio run --target upload
```

---

## 6. Configuration Reference (config.h)

All site-specific settings live in `include/config.h`. This file is excluded from git.

### WiFi

```cpp
#define WIFI_SSID         "YourNetworkName"
#define WIFI_PASSWORD     "YourPassword"
```

**Must be 2.4 GHz.** The ESP32 does not support 5 GHz. The firmware retries the connection automatically on every `loop()` pass if the connection drops.

### MQTT

```cpp
#define MQTT_SERVER       "192.168.1.100"   // Static LAN IP of your Home Assistant host
#define MQTT_PORT         1883              // Default Mosquitto port (plaintext)
#define MQTT_USER         "mqtt_user"
#define MQTT_PASSWORD     "your-mqtt-password"
#define MQTT_CLIENT_ID    "pentair-controller"  // Must be unique on the broker
#define MQTT_TOPIC_PREFIX "pentair"             // All topics: pentair/subsystem/field
```

- `MQTT_SERVER` — The IP of your Home Assistant machine, not `localhost`. Use the static IP you assigned.
- `MQTT_CLIENT_ID` — If you run multiple ESP32 devices on the same broker, each must have a unique client ID. Duplicate IDs cause disconnect loops.
- `MQTT_TOPIC_PREFIX` — Change this if you have a name collision with other MQTT devices.

### RS485 Pins

```cpp
#define RS485_RX_PIN      18   // MAX485 RO → ESP32
#define RS485_TX_PIN      17   // ESP32 → MAX485 DI
#define RS485_DE_RE_PIN   16   // Direction control (DE + RE tied together)
#define RS485_UART_NUM    1    // Must be UART1; UART0 is used by USB CDC
```

Change these only if your wiring uses different GPIO pins. `RS485_UART_NUM` **must** stay at `1` on an ESP32-S3 with USB CDC enabled.

### Device Addresses

```cpp
#define ADDR_BROADCAST    0x0F  // A5 broadcast
#define ADDR_INTELLITOUCH 0x10  // IntelliConnect controller
#define ADDR_PUMP         0x60  // IntelliFlo VS pump
#define ADDR_CHLORINATOR  0x50  // IntelliChlor
#define ADDR_THIS_DEVICE  0x20  // Our ESP32 (wireless remote address range)
```

These are standard Pentair RS485 addresses. You should not need to change them for a typical IntelliConnect + IntelliFlo + IntelliChlor installation.

### Pump Speed Circuit Numbers

```cpp
#define PUMP_CIRCUIT_STANDARD  2    // Program 1 (~1650 RPM)
#define PUMP_CIRCUIT_HEAT      3    // Program 2 (~2300 RPM) — confirmed from bus capture
#define PUMP_CIRCUIT_CLEANER   4    // Program 3 (~2400 RPM) — confirmed from bus capture
#define PUMP_CIRCUIT_TEST      5    // Program 4 (~1670 RPM)
```

These are the MODE values sent to the pump via `ACTION_PUMP_MODE` (0x05). The relationship is:
**MODE value = pump program index + 1** (program 1 = MODE 2, program 2 = MODE 3, etc.)

To discover the MODE values for your installation, switch speeds in the IntelliConnect app while watching for `[PUMP_MODE]` lines in the UDP log. The displayed `mode=0xXX` value is the circuit number to use.

```cpp
#define PUMP_RPM_STANDARD  1650
#define PUMP_RPM_HEAT      2300
#define PUMP_RPM_CLEANER   2400
#define PUMP_RPM_TEST      1670
```

These RPM values are used only for display (mapping live RPM back to a preset name in HA). Update them if you reprogramme your pump speeds in the IntelliConnect app.

### Pump Speed to Preset Name Mapping

Live RPM is mapped to a named preset using midpoint boundaries:

| Preset | RPM Range |
|--------|-----------|
| Stop | 0 |
| Standard | 1 – 1660 RPM |
| Test | 1661 – 1985 RPM |
| Heat | 1986 – 2350 RPM |
| Cleaner | > 2350 RPM |

If your Standard and Test speeds differ significantly from these defaults, adjust the thresholds in `pentair_protocol.cpp`.

### OTA

```cpp
#define OTA_HOSTNAME  "pentair-controller"
#define OTA_PASSWORD  "change-me"
```

**Change `OTA_PASSWORD`** before deploying. Also update `--auth=` in `platformio.ini` to match.

### NTP / Time

```cpp
#define NTP_SERVER         "pool.ntp.org"
#define NTP_UTC_OFFSET_SEC (-7 * 3600)   // PDT
#define NTP_DST_OFFSET_SEC 0
```

Time is used only for human-readable timestamps in heartbeat log messages.

| Timezone | Value |
|----------|-------|
| PDT (UTC−7) | `-7 * 3600` |
| PST (UTC−8) | `-8 * 3600` |
| MDT (UTC−6) | `-6 * 3600` |
| EDT (UTC−4) | `-4 * 3600` |
| EST (UTC−5) | `-5 * 3600` |

### Timing Constants

```cpp
#define WIFI_CONNECT_TIMEOUT_MS   10000   // Give up waiting for WiFi after 10 s (retries next loop)
#define MQTT_RECONNECT_INTERVAL   5000    // Min ms between broker reconnect attempts
#define RS485_TX_DELAY_US         100     // Wait after asserting DE/RE before writing first bit
#define RS485_TX_COMPLETE_WAIT_MS 10      // Hold DE/RE after flush() while last bits propagate
```

These are tuned for the MAX485 and do not normally need adjustment.

---

## 7. Code Walkthrough

### File Layout

```
esp32-pentair-intelliconnect/
├── include/
│   ├── config.h             ← Site-specific settings (gitignored; create from .example)
│   ├── config.h.example     ← Template — copy to config.h and fill in values
│   ├── pentair_protocol.h   ← All protocol types, enums, structs, function declarations
│   ├── rs485.h              ← RS485 driver class
│   ├── mqtt_handler.h       ← MQTT client wrapper class
│   └── udp_log.h            ← UDP broadcast logger
├── src/
│   ├── main.cpp             ← Arduino setup/loop, WiFi, OTA, MQTT command dispatch
│   ├── pentair_protocol.cpp ← Protocol parsers, packet builders, PoolState updaters
│   ├── rs485.cpp            ← UART driver, ring buffer, packet dispatch
│   ├── mqtt_handler.cpp     ← MQTT publish, HA discovery, reconnection management
│   └── udp_log.cpp          ← UDP broadcast logger implementation
├── platformio.ini           ← PlatformIO build and OTA config
├── .gitignore               ← Excludes config.h, .pio/, .vscode/
└── README.md
```

### main.cpp

The top-level application file.

**`setup()`**
1. Initializes UART and RS485 driver
2. Connects to WiFi
3. Starts the UDP logger
4. Configures and starts ArduinoOTA
5. Syncs time via NTP
6. Connects to the MQTT broker and registers the command callback

**`loop()`** — runs continuously at several hundred Hz:
1. Checks WiFi and reconnects if dropped
2. Calls `ArduinoOTA.handle()` to service OTA upload requests
3. Calls `mqtt.loop()` — manages MQTT reconnection and processes incoming messages
4. Calls `rs485.loop()` — drains the UART FIFO and runs the protocol parsers
5. Checks `rs485.hasPacket()` and processes any complete A5 packet
6. Checks `rs485.hasChlorPacket()` and processes any complete IntelliChlor packet
7. Updates `poolState.lastRs485Activity` from the RS485 driver's activity timestamp
8. Three periodic tasks:
   - Every **10 seconds**: re-publish all PoolState to MQTT (refreshes HA even if nothing changed)
   - Every **30 seconds**: actively query the chlorinator for salt/temp data
   - Every **5 seconds**: emit a heartbeat UDP log line with uptime, IP, MQTT status, and byte count

**`handleMqttCommand(topic, payload)`**

Routes incoming MQTT commands:
- `pentair/restart` → `ESP.restart()`
- `pentair/pump/speed/set` → Sends `ACTION_PUMP_MODE` + `ACTION_PUMP_RUN` to the pump, then immediately updates `poolState.pumpSpeedPreset` and forces an MQTT publish so HA reflects the change without waiting for the next pump STATUS packet
- `pentair/chlorinator/set` → Sends `CHLOR_SET_OUTPUT` to the chlorinator

### pentair_protocol.cpp / .h

Pure data processing — no I/O, no hardware access. All functions are in the `Pentair::` namespace.

**Packet parsing**

`parsePacket()` — Streaming A5 scanner. Scans the ring buffer for the 4-byte preamble `FF 00 FF A5`, reads the header, verifies the 16-bit checksum (sum of all bytes from A5 through the last data byte), and populates a `PentairPacket` struct. Sets `bytesConsumed` so the caller can advance the buffer.

`parseChlorPacket()` — Same approach for IntelliChlor frames (`10 02 ... checksum 10 03`). Verifies the 8-bit checksum: `(18 + sum(dst + action + payload)) & 0xFF`.

**State processing**

`processPacket(packet, poolState)` — Updates the `PoolState` struct from an A5 packet:
- `ACTION_PUMP_STATUS` (0x07) from `ADDR_PUMP`: extracts RPM from bytes [5:6], watts from bytes [3:4]; derives `pumpSpeedPreset` from RPM using midpoint thresholds
- `ACTION_STATUS_RESPONSE` (0x02) from `ADDR_INTELLITOUCH`: reads circuit bitmask for pool/light/cleaner state; reads heater active flag

`processChlorPacket(cpkt, poolState)` — Updates `PoolState` from an IntelliChlor packet:
- `CHLOR_STATUS_RESP` / `CHLOR_OUTPUT_RESP`: output %, salt PPM (raw × 50), warning flags
- `CHLOR_EXTENDED_STATUS` (0x16): water temperature from byte [0] — authoritative temperature source

**Packet builders**

`buildPacket()` — Serializes an A5 frame: preamble `FF 00 FF A5`, header, data, 16-bit checksum.

`buildChlorQuery()` — Builds a `CHLOR_GET_STATUS` (0x00) query to the chlorinator.

`buildChlorSetOutput()` — Builds a `CHLOR_SET_OUTPUT` (0x11) command with a 0–100% value.

**PumpSpeed enum**

```cpp
enum PumpSpeed : uint8_t {
    PUMP_SPEED_STOP     = 0,   // RPM == 0
    PUMP_SPEED_STANDARD = 1,   // RPM   1–1660
    PUMP_SPEED_TEST     = 4,   // RPM 1661–1985
    PUMP_SPEED_HEAT     = 2,   // RPM 1986–2350
    PUMP_SPEED_CLEANER  = 3,   // RPM > 2350
};
```

The enum index matches the `speedNames[]` array in `mqtt_handler.cpp` (`{"Stop", "Standard", "Heat", "Cleaner", "Test"}`), which is what gets published to HA.

### rs485.cpp / .h

Hardware driver for the half-duplex RS485 bus via UART1.

**`begin(baud)`** — Configures `HardwareSerial` on the specified UART and GPIO pins; sets DE/RE pin LOW (receive mode).

**`loop()`** — Called every Arduino loop iteration:
1. Reads all available bytes from the UART hardware FIFO into the 256-byte ring buffer
2. Skips leading bytes that cannot start a valid frame (neither `0xFF` for A5 nor `0x10` for IntelliChlor)
3. Tries `parsePacket()` or `parseChlorPacket()` based on the leading byte
4. On success: stores the parsed packet, sets the ready flag, advances the buffer by `bytesConsumed`
5. On failure (incomplete frame): leaves the buffer unchanged and waits for more bytes

**`sendPacket(data, len)`** — Half-duplex transmit sequence:
1. Assert DE/RE HIGH (transmit mode)
2. Wait `RS485_TX_DELAY_US` (100 µs) for the MAX485 output driver to settle
3. Write all bytes via `_serial->write()`
4. Call `_serial->flush()` — blocks until the hardware shift register is empty
5. Wait `RS485_TX_COMPLETE_WAIT_MS` (10 ms) for the last bits to propagate off the wire
6. Assert DE/RE LOW (receive mode)

**Statistics:** The driver tracks `_bytesReceived`, `_packetsReceived`, and `_lastActivityMillis` (timestamp of the most recent byte received). `_lastActivityMillis` is used by the `rs485_alive` health sensor.

### mqtt_handler.cpp / .h

MQTT client wrapper built on PubSubClient.

**Connection management**

`loop()` — Backs off reconnection attempts by `MQTT_RECONNECT_INTERVAL` (5 s). On successful reconnect, calls `subscribe()` and `publishDiscovery()`, and sets `_firstPublish = true` to force an immediate state publish.

**`publishState(poolState)`**

Rate-limited to once every **2 seconds** to avoid flooding the broker during packet bursts. Always fires immediately after a reconnect. Publishes:
- Water temp (suppressed if 0 — lets HA retain the last valid value)
- Pump state, RPM, watts, speed preset name
- Chlorinator output, salt PPM (suppressed if 0), active/low-salt/check-cell flags
- `rs485_alive` / `pentair_alive` binary connectivity (ON if activity within 60 s)

All publishes use the **retain** flag.

**`forceNextPublish()`** — Resets the rate-limit timer so the next `publishState()` call fires immediately. Called by `handleMqttCommand()` after sending a pump speed command, so HA's select box updates without waiting up to 2 seconds for the pump to report back its new RPM.

**`publishDiscovery()`**

Publishes HA MQTT auto-discovery JSON for every entity on every (re)connect. Also publishes **empty retained payloads** to a hardcoded list of stale discovery topics from previous firmware versions, removing those entities from HA automatically.

### Data Flow

```
RS485 Bus (9600 baud 8N1)
        │
        ▼
   RS485::loop()          — raw bytes into 256-byte ring buffer
        │
        ├── parsePacket()         A5 frames → PentairPacket
        └── parseChlorPacket()    IntelliChlor frames → ChlorPacket
                │
                ▼
   main.cpp loop()
        ├── processPacket()        A5 → PoolState (RPM, watts, circuits, heater)
        └── processChlorPacket()   IntelliChlor → PoolState (salt, temp, output%)
                │
                ▼
   MqttHandler::publishState()    PoolState fields → retained MQTT topics → HA
                │
                ▼
   Home Assistant (sensors, select, binary sensors, button)
                │
                ▼
   User sends command from HA dashboard
                │
                ▼
   MQTT broker → callback → handleMqttCommand() → RS485::sendPacket()
```

### Pump Speed Change — How It Works

The firmware replicates exactly what the IntelliConnect does when changing pump speed from its app:

1. Send `ACTION_PUMP_MODE` (0x05) with the circuit number (e.g. `0x03` for Heat)
2. Wait 100 ms
3. Send `ACTION_PUMP_RUN` (0x06) with `0x0A` (run)

No remote-control handshake (`ACTION_PUMP_REMOTE`) is needed — using it causes the IntelliConnect to detect a bus conflict and restart its control sequence.

The ESP32 immediately updates `poolState.pumpSpeedPreset` optimistically after sending these commands (before the pump reports back with a STATUS packet). This ensures HA reflects the requested speed instantly rather than waiting up to 2 seconds for pump telemetry to catch up.

**Key behavior:** The IntelliConnect sends `ACTION_PUMP_MODE` only at startup or when the user changes the speed in its app. It does **not** re-send MODE on every polling cycle. This means a MODE command from the ESP32 sticks for the duration of the pool run session — the IntelliConnect's periodic RUN keepalives preserve the mode without overriding it.

---

## 8. Pentair Protocol Reference

### A5 Protocol (Controller / Pump / Heater)

Used by the IntelliConnect (0x10), IntelliFlo pump (0x60), and gas heater. 9600 baud, 8N1, no flow control.

**Frame structure:**

```
[FF][00][FF][A5][proto][dst][src][action][len][data × len][ckH][ckL]
 ←── 3-byte preamble ──→ ←──────────── checksum covers these bytes ──────────────→
```

| Field | Bytes | Description |
|-------|-------|-------------|
| Preamble | 3 | Always `FF 00 FF` |
| A5 marker | 1 | Always `0xA5` — checksum coverage starts here |
| Protocol | 1 | Always `0x00` on IntelliConnect bus |
| Dst | 1 | Destination device address |
| Src | 1 | Source device address |
| Action | 1 | Command/response type code |
| Len | 1 | Number of data bytes |
| Data | 0–64 | Payload (Len bytes) |
| Checksum | 2 | 16-bit sum of bytes from A5 through last data byte (big-endian) |

**A5 Action Codes:**

| Code | Name | Direction | Description |
|------|------|-----------|-------------|
| 0x01 | `ACTION_PUMP_WRITE` | Controller → Pump | Write a named register to the pump |
| 0x02 | `ACTION_STATUS_RESPONSE` | Controller → Broadcast | Equipment status (circuits, temps, heat mode) |
| 0x04 | `ACTION_PUMP_REMOTE` | Any → Pump | Grant (0xFF) or release (0x00) remote control |
| 0x05 | `ACTION_PUMP_MODE` | Controller/ESP32 → Pump | Select operating mode (speed program) |
| 0x06 | `ACTION_PUMP_RUN` | Controller/ESP32 → Pump | Run (0x0A) or stop (0x04) |
| 0x07 | `ACTION_PUMP_STATUS` | Pump → Controller | Telemetry: RPM, watts, operating state |
| 0x08 | `ACTION_HEAT_STATUS` | Controller → Broadcast | Setpoints (not seen on this bus) |
| 0x86 | `ACTION_SET_CIRCUIT` | External → Controller | Circuit on/off — **ignored by IntelliConnect** |
| 0x88 | `ACTION_SET_HEAT_MODE` | External → Controller | Heat setpoint — **ignored by IntelliConnect** |
| 0xC8 | `ACTION_STATUS_REQUEST` | Any → Controller | Request a status broadcast |

**`ACTION_PUMP_STATUS` (0x07) data layout:**

| Bytes | Content |
|-------|---------|
| [3:4] | Pump watts (big-endian uint16) |
| [5:6] | Pump RPM (big-endian uint16) |

**`ACTION_STATUS_RESPONSE` (0x02) data layout:**

| Byte | Content |
|------|---------|
| [2:3] | Circuit bitmask (uint16). Bit (N−1) = circuit N on/off. |
| [14] | Water temperature °F (unreliable on this bus — use IntelliChlor instead) |
| [16] | Heater state: `0x00` = off, `0x20` = burner actively firing |
| [18] | Air temperature °F |
| [22] | Heat mode (lower 2 bits for pool): 0=off, 1=heater, 2=solar-pref, 3=solar-only |

**Circuit bit positions (installation-specific):**

| Circuit | Bit (N−1) | Label |
|---------|-----------|-------|
| 6 | 5 | Pool (main circulation) |
| 2 | 1 | Pool light |
| 5 | 4 | Cleaner / booster pump |

### IntelliChlor Protocol (Salt Chlorinator)

Separate framing on the same physical RS485 bus. Frames are wrapped in `[10 02] ... [checksum] [10 03]`.

**Frame structure:**

```
[10][02][dst][action][payload...][checksum][10][03]
```

| Field | Bytes | Description |
|-------|-------|-------------|
| Start | 2 | Always `10 02` |
| Dst | 1 | Destination address (`0x50` = chlorinator) |
| Action | 1 | Command/response code |
| Payload | variable | Action-specific data |
| Checksum | 1 | `(18 + sum(dst + action + payload)) & 0xFF` |
| End | 2 | Always `10 03` |

**IntelliChlor Action Codes:**

| Code | Name | Description |
|------|------|-------------|
| 0x00 | `CHLOR_GET_STATUS` | Query: request current status |
| 0x01 | `CHLOR_STATUS_RESP` | Response: [output%][salt_raw][flags] |
| 0x11 | `CHLOR_SET_OUTPUT` | Command: set output % (0–100) |
| 0x12 | `CHLOR_OUTPUT_RESP` | Ack to SET_OUTPUT: same layout as STATUS_RESP |
| 0x14 | `CHLOR_GET_VERSION` | Query: request firmware version |
| 0x03 | `CHLOR_VERSION_RESP` | Response: firmware version string |
| 0x16 | `CHLOR_EXTENDED_STATUS` | Extended status: byte[0] = water temp °F |

**`CHLOR_STATUS_RESP` / `CHLOR_OUTPUT_RESP` data layout:**

| Byte | Content |
|------|---------|
| [0] | Output percentage (0–100) |
| [1] | Salt PPM raw value → `ppm = raw × 50` |
| [2] | Flags: bit 2 (`0x04`) = low salt; bit 1 (`0x02`) = inspect cell |

**`CHLOR_EXTENDED_STATUS` (0x16) data layout:**

| Byte | Content |
|------|---------|
| [0] | Water temperature in °F — **the authoritative temperature source on this bus** |
| [1] | Output percentage |
| [2:3] | Additional status flags |

**Salt PPM calculation:** `ppm = raw_byte × 50` (e.g. raw `0x3C` = 60 → 3000 PPM)

### RS485 Device Addresses

| Address | Device |
|---------|--------|
| `0x0F` | A5 broadcast |
| `0x10` | IntelliConnect controller (also IntelliTouch) |
| `0x20` | Wireless remote / external device ← this ESP32 |
| `0x50` | IntelliChlor salt chlorinator |
| `0x60` | IntelliFlo VS pump |

---

## 9. MQTT Topics Reference

All topics use the prefix `pentair` (configurable via `MQTT_TOPIC_PREFIX`). All state publishes use the **retain** flag.

### State Topics (published by ESP32)

| Topic | Payload | Source |
|-------|---------|--------|
| `pentair/water_temp` | Integer °F | IntelliChlor CHLOR_EXTENDED_STATUS; suppressed if 0 |
| `pentair/pump/state` | `ON` / `OFF` | RPM > 0 |
| `pentair/pump/rpm` | Integer | A5 PUMP_STATUS bytes[5:6] |
| `pentair/pump/watts` | Integer | A5 PUMP_STATUS bytes[3:4] |
| `pentair/pump/speed/state` | `Stop` / `Standard` / `Heat` / `Cleaner` / `Test` | RPM mapped to nearest preset |
| `pentair/chlorinator/output` | Integer 0–100 | IntelliChlor output % |
| `pentair/chlorinator/salt_ppm` | Integer | IntelliChlor raw salt × 50; suppressed if 0 |
| `pentair/chlorinator/state` | `ON` / `OFF` | Chlorinator active flag |
| `pentair/chlorinator/low_salt` | `ON` / `OFF` | Low salt warning |
| `pentair/chlorinator/check_cell` | `ON` / `OFF` | Inspect cell warning |
| `pentair/rs485_alive` | `ON` / `OFF` | ON if any RS485 byte seen within last 60 s |
| `pentair/pentair_alive` | `ON` / `OFF` | ON if A5 pump packet seen within last 60 s |

### Command Topics (subscribed by ESP32)

| Topic | Accepted Payloads | Effect |
|-------|------------------|--------|
| `pentair/pump/speed/set` | `Stop`, `Standard`, `Heat`, `Cleaner`, `Test` | Sends ACTION_PUMP_MODE + ACTION_PUMP_RUN to pump |
| `pentair/chlorinator/set` | `0`–`100` (integer string) | Sends CHLOR_SET_OUTPUT to chlorinator |
| `pentair/restart` | `PRESS` | Calls `ESP.restart()` |

### Health Sensor States in HA

| HA State | Meaning |
|----------|---------|
| **Connected** | Activity seen within last 60 seconds |
| **Disconnected** | No activity for > 60 seconds |
| **Unavailable** | ESP32 hasn't published for > 30 s (ESP32 is offline) |

The "Unavailable" state is enforced by `expire_after: 30` in the MQTT discovery config — HA handles it automatically.

---

## 10. Home Assistant Integration

### Step 1 — Add the MQTT Integration

**Settings → Devices & Services → Add Integration → MQTT**

Enter your broker details:
- Host: your Home Assistant LAN IP (or `127.0.0.1` if Mosquitto runs on the same machine)
- Port: `1883`
- Username and password matching `config.h`

### Step 2 — Auto-Discovery

When the ESP32 connects to the broker it publishes HA MQTT auto-discovery payloads. No YAML configuration is needed. All entities appear under a single device card: **"Pentair Pool Controller"** (manufacturer: Pentair, model: IntelliConnect).

**Entities created automatically:**

| Type | Entity | Notes |
|------|--------|-------|
| Sensor | Water Temp | °F, device class: temperature |
| Sensor | Pump | ON/OFF |
| Sensor | Pump RPM | RPM |
| Sensor | Pump Power | Watts, device class: power |
| Sensor | Chlorinator Output | % |
| Sensor | Salt Level | ppm |
| Sensor | Chlorinator Active | ON/OFF |
| Sensor | Salt Low | ON/OFF |
| Sensor | Check Cell | ON/OFF |
| Select | Pump Speed | Stop / Standard / Heat / Cleaner / Test |
| Binary Sensor | RS485 Bus | device class: connectivity |
| Binary Sensor | Pentair Pump | device class: connectivity |
| Button | Restart ESP32 | Triggers ESP.restart() |

### Example Lovelace Dashboard

```yaml
type: entities
title: Pool
entities:
  - entity: sensor.water_temp
  - entity: sensor.pump_rpm
  - entity: sensor.pump_power
  - entity: select.pump_speed
  - entity: sensor.chlorinator_output
  - entity: sensor.salt_level
  - entity: binary_sensor.rs485_bus
  - entity: binary_sensor.pentair_pump
  - entity: button.restart_esp32
```

### Example Automation — Salt Low Alert

```yaml
alias: Pool Salt Low Alert
trigger:
  - platform: state
    entity_id: binary_sensor.salt_low
    to: "on"
action:
  - service: notify.mobile_app_your_phone
    data:
      message: "Pool salt level is low. Check chlorinator."
```

### Example Automation — Night Speed Reduction

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
      option: Standard
```

---

## 11. OTA Firmware Updates

After the initial USB flash, all subsequent updates can be done wirelessly.

### Update command

```bash
pio run --target upload
```

PlatformIO reads `upload_port` and `upload_flags` from `platformio.ini` and uses the ArduinoOTA protocol with your configured password.

### Manual port override

```bash
pio run --target upload --upload-port 192.168.x.x
```

### Security note

`OTA_PASSWORD` in `config.h` and `--auth=` in `platformio.ini` must match. Change the password from the default before deploying — anyone on your LAN who knows the password and device IP can flash arbitrary firmware.

---

## 12. UDP Debug Logging

All diagnostic messages are broadcast to the LAN on **UDP port 4210** simultaneously with USB Serial output. This allows wireless monitoring once the device is physically installed in the pool equipment enclosure.

### Receiving logs

**Windows (PowerShell):**
```powershell
$udp = [System.Net.Sockets.UdpClient]::new(4210)
while ($true) {
    $ep = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
    $data = $udp.Receive([ref]$ep)
    [System.Text.Encoding]::UTF8.GetString($data)
}
```

**Linux / macOS:**
```bash
nc -u -l 4210
```

**Python (cross-platform):**
```python
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('', 4210))
while True:
    print(s.recvfrom(1024)[0].decode(), end='')
```

### Log prefix reference

| Prefix | Meaning |
|--------|---------|
| `[WiFi]` | WiFi connection events |
| `[MQTT]` | MQTT connect, subscribe, publish |
| `[OTA]` | OTA update progress |
| `[NTP]` | Time sync |
| `[RS485]` | UART initialization |
| `[PKT]` | A5 packet hex dump (src, dst, action, first 16 data bytes) |
| `[CHLOR]` | IntelliChlor frame hex dump |
| `[PUMP_STATUS]` | Pump RPM and watts (logged only when RPM changes) |
| `[PUMP_MODE]` | MODE command seen on bus — use this to discover circuit numbers for your install |
| `[PUMP_RUN]` | RUN/STOP command seen on bus |
| `[PUMP_WRITE]` | Register write to pump |
| `[PUMP_REG]` | Register read response from pump |
| `[PUMP_CTRL]` | Remote control grant/release seen on bus |
| `[CMD]` | Incoming MQTT command being processed |
| `[ALIVE]` | Heartbeat every 5 s: timestamp, uptime, IP, MQTT status, total rx bytes |

### Sample output

```
[ALIVE] 2026-05-05 14:23:01 uptime=3600s ip=192.168.86.118 mqtt=OK rx_bytes=142880
[PKT] src=0x60 dst=0x10 action=0x07 len=15 | 0A 02 02 02 8E 08 FC ...
[PUMP_STATUS] rpm=2300 watts=654
[CHLOR] action=0x16 len=4 | 59 46 00 00
[CMD] pentair/pump/speed/set = Heat
[CMD] Pump speed set to Heat (circuit 0x03)
```

---

## 13. Troubleshooting

### Device won't connect to WiFi

- Verify `WIFI_SSID` and `WIFI_PASSWORD` in `config.h` (case-sensitive).
- The ESP32 only supports **2.4 GHz** networks.
- Try a power cycle of both the router and the ESP32.

### MQTT "Failed, rc=X"

| Code | Cause | Fix |
|------|-------|-----|
| −2 | Can't reach broker | Verify `MQTT_SERVER` IP; confirm port 1883 is reachable |
| −4 | Connection timeout | Check Mosquitto is running; check router firewall on port 1883 |
| 4 | Auth failed | Verify `MQTT_USER` / `MQTT_PASSWORD` in `config.h` |
| 5 | Not authorized | Check Mosquitto ACL rules allow the user on `pentair/#` |

### No entities in Home Assistant

1. In HA **Developer Tools → MQTT**, subscribe to `pentair/#`. Messages should arrive every few seconds.
2. If no messages: check UDP log for `[MQTT] Connected!` and `[MQTT] Published HA discovery configs`.
3. If messages arrive but no entities appear: power-cycle the ESP32 to re-send discovery payloads.

### Pump speed command does nothing

1. The command only works while the pump is running. If the pool circuit is OFF, IntelliConnect overrides any speed change immediately.
2. Check the UDP log for `[CMD] Pump speed set to X (circuit 0xXX)`. If absent, verify `MQTT_TOPIC_PREFIX` and subscription.
3. If the command is logged but RPM doesn't change, RS485 A/B wires may be swapped — try reversing them.

### Water temp shows 0 or never updates

Water temperature comes from `CHLOR_EXTENDED_STATUS` (0x16) from the IntelliChlor. Zero values are suppressed; HA retains the last valid reading.

- Check the UDP log for `[CHLOR] action=0x16` entries.
- The ESP32 queries the chlorinator every 30 seconds. If no `[CHLOR]` lines appear at all, the RS485 connection may not be working.

### HA shows "Unavailable" for binary sensors

The ESP32 publishes a periodic state update every 10 seconds. "Unavailable" means no publish has arrived in 30 seconds — the device is offline, crashed, or disconnected from MQTT/WiFi. Use the Restart button in HA if still visible, or power-cycle the device.

### OTA upload fails — "No response from device"

- Ping the device IP to confirm reachability.
- Check UDP log for `[ALIVE]` heartbeats — if absent, the device is stuck.
- Verify `upload_port` in `platformio.ini` matches the device's actual IP.

### OTA upload fails — "Authentication failed"

`--auth=` in `platformio.ini` must exactly match `OTA_PASSWORD` in `config.h`.

### RS485 receives bytes but no packets parse

- **Check polarity:** A and B swapped is the most common cause. Reverse them.
- **Check MAX485 VCC:** Must be 5V, not 3.3V.
- **Check GND:** ESP32 and MAX485 must share a common ground.
- **Check baud rate:** `PENTAIR_BAUD_RATE` must be 9600.

### Salt PPM reads wrong value

Salt PPM = `raw_byte × 50`. If the displayed value disagrees with the IntelliConnect app or a manual test kit, the IntelliChlor cell may need calibration — a Pentair service procedure unrelated to this firmware.

---

*Built for a Pentair IntelliConnect + IntelliFlo VS + IntelliChlor installation. Protocol details reverse-engineered from RS485 bus captures.*
