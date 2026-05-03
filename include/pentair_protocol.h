/*
 * pentair_protocol.h
 *
 * Types, constants, and function declarations for the two Pentair bus protocols:
 *
 *   A5 (pump / controller / heater):
 *     Frame: [FF 00 FF A5] [proto] [dst] [src] [action] [len] [data...] [ckH ckL]
 *     Checksum: 16-bit sum of every byte from A5 through the last data byte.
 *
 *   IntelliChlor (salt chlorinator, address 0x50):
 *     Frame: [10 02] [dst] [action] [payload...] [checksum] [10 03]
 *     Checksum: (18 + sum of dst + action + payload bytes) & 0xFF
 *
 * IntelliConnect (0x10) is the pool controller. It communicates with the pump
 * (0x60) and chlorinator (0x50) directly, but does NOT accept A5 SET commands
 * from external devices on the RS485 bus. Circuit on/off and setpoint commands
 * aimed at 0x10 are silently ignored by the IntelliConnect hardware.
 *
 * Our device (ADDR_THIS_DEVICE = 0x20) acts as a wireless remote. It can
 * command the pump and chlorinator directly, and it passively reads all
 * traffic to build the PoolState.
 */

#pragma once

#include <Arduino.h>

// A5 preamble ends with this byte; checksum coverage starts here.
static const uint8_t PENTAIR_HEADER      = 0xA5;
static const size_t  PENTAIR_MAX_DATA_LEN   = 64;
// Minimum A5 packet: 3-byte preamble + A5 + proto + dst + src + action + len + 2-byte checksum = 11
static const size_t  PENTAIR_MIN_PACKET_SIZE = 11;

// ============================================================
// A5 action codes
// ============================================================
// Values 0x01-0x0F are sent TO devices (commands/requests).
// Values 0x80+ are sent TO the controller (SET commands) — note that
// IntelliConnect silently ignores 0x86 and 0x88 from external devices.
enum PentairAction : uint8_t {
    ACTION_PUMP_WRITE      = 0x01,  // Write a named register to the pump (used for speed control)
    ACTION_STATUS_RESPONSE = 0x02,  // Controller status broadcast (circuits, temps, heat mode)
    ACTION_PUMP_REMOTE     = 0x04,  // Grant/release remote control of pump (0xFF=remote, 0x00=local)
    ACTION_PUMP_MODE       = 0x05,  // Set pump operating mode (normal/feature/etc.)
    ACTION_PUMP_RUN        = 0x06,  // Set pump run/stop state
    ACTION_PUMP_STATUS     = 0x07,  // Pump telemetry (RPM in data[5:6], watts in data[3:4])
    ACTION_HEAT_STATUS     = 0x08,  // Heat/temperature status — setpoints; NOT seen on this bus
    ACTION_SET_CIRCUIT     = 0x86,  // Turn a numbered circuit on/off — ignored by IntelliConnect
    ACTION_SET_HEAT_MODE   = 0x88,  // Set heat mode/setpoint — ignored by IntelliConnect
    ACTION_STATUS_REQUEST  = 0xC8,  // Poll controller for an ACTION_STATUS_RESPONSE broadcast
};

// ============================================================
// IntelliChlor action codes (carried inside [10 02 ... 10 03] frames)
// ============================================================
// The IntelliChlor protocol is separate from A5 and uses a different
// framing scheme. The controller (IntelliConnect) polls the chlorinator
// roughly every 30 seconds; the ESP32 also polls independently.
enum ChlorAction : uint8_t {
    CHLOR_GET_STATUS      = 0x00,  // Query: request current chlorinator status
    CHLOR_STATUS_RESP     = 0x01,  // Response to CHLOR_GET_STATUS: [output%][salt_raw][flags]
    CHLOR_SET_OUTPUT      = 0x11,  // Command: set chlorine output percentage (0-100)
    CHLOR_OUTPUT_RESP     = 0x12,  // Acknowledgement to CHLOR_SET_OUTPUT: [output%][salt_raw][flags]
    CHLOR_GET_VERSION     = 0x14,  // Query: request firmware version string
    CHLOR_VERSION_RESP    = 0x03,  // Response to CHLOR_GET_VERSION
    // CHLOR_EXTENDED_STATUS is the ONLY reliable water temperature source on this
    // installation. The cell has its own thermistor; byte[0] = water temp in °F.
    CHLOR_EXTENDED_STATUS = 0x16,  // Extended status: byte[0]=water temp °F, byte[1]=output%, bytes[2-3]=status flags
};

// ============================================================
// Circuit IDs (1-based bit positions in the A5 STATUS_RESPONSE circuit bitmask)
// ============================================================
// The circuit bitmask in ACTION_STATUS_RESPONSE data[2:3] uses 1-based bit
// positions: circuit N occupies bit (N-1). These IDs are installation-specific;
// they reflect how this pool's controller was programmed and may differ on
// other systems. Circuit on/off SET commands (0x86) are ignored by IntelliConnect.
enum PentairCircuit : uint8_t {
    CIRCUIT_POOL       = 6,  // Main pool circulation pump/filter circuit
    CIRCUIT_POOL_LIGHT = 2,  // Pool light (relay on IntelliConnect)
    CIRCUIT_CLEANER    = 5,  // Automatic cleaner / booster pump circuit
};

// ============================================================
// Heat mode values (occupy the lower 2 bits of the heat-mode byte)
// ============================================================
enum HeatMode : uint8_t {
    HEAT_OFF        = 0,
    HEAT_HEATER     = 1,  // Gas heater
    HEAT_SOLAR_PREF = 2,  // Solar preferred (heater backup)
    HEAT_SOLAR_ONLY = 3,
};

// ============================================================
// Pump speed presets — friendly labels mapped from observed RPM ranges
// ============================================================
// These are display/reporting labels derived from live RPM readings; they do
// not correspond 1:1 to pump programs. The RPM boundaries are chosen to match
// this installation's four programs (1000 / 1640 / 2315 / 3450 RPM).
// The enum index also matches the speedNames[] array in mqtt_handler.cpp.
enum PumpSpeed : uint8_t {
    PUMP_SPEED_STOP   = 0,  // RPM == 0
    PUMP_SPEED_LOW    = 1,  // RPM  1 – 1200  (Program 1 on this system: ~1000 RPM)
    PUMP_SPEED_MEDIUM = 2,  // RPM  1201–2000 (Program 2 on this system: ~1640 RPM)
    PUMP_SPEED_HIGH   = 3,  // RPM  2001–2900 (Program 3 on this system: ~2315-2420 RPM)
    PUMP_SPEED_MAX    = 4,  // RPM  > 2900    (Program 4 on this system: up to ~3450 RPM)
};

// ============================================================
// Parsed A5 packet
// ============================================================
struct PentairPacket {
    uint8_t  protocol;                 // Always 0x00 on IntelliConnect bus traffic
    uint8_t  dst;                      // Destination address (e.g. 0x60 = pump)
    uint8_t  src;                      // Source address (e.g. 0x10 = controller)
    uint8_t  action;                   // See PentairAction enum
    uint8_t  dataLen;                  // Number of data bytes that follow
    uint8_t  data[PENTAIR_MAX_DATA_LEN]; // Payload bytes (dataLen valid bytes)
    uint16_t checksum;                 // Received checksum (already verified by parser)
    bool     valid;                    // True only if checksum passed
};

// ============================================================
// Parsed IntelliChlor packet
// ============================================================
struct ChlorPacket {
    uint8_t dst;      // 0x50 = chlorinator address
    uint8_t action;
    uint8_t dataLen;
    uint8_t data[16];
    bool    valid;
};

// ============================================================
// Aggregated equipment state — updated by processPacket / processChlorPacket
// ============================================================
struct PoolState {
    // Circuit states (from A5 ACTION_STATUS_RESPONSE bitmask)
    bool    poolOn;
    bool    poolLightOn;
    bool    cleanerOn;

    // Temperatures (°F)
    // waterTemp comes from IntelliChlor CHLOR_EXTENDED_STATUS, not from the controller
    uint8_t waterTemp;
    uint8_t airTemp;
    uint8_t poolSetpoint;

    // Heater (gas only — no solar on this system)
    bool    heaterOn;      // Heat mode != HEAT_OFF
    bool    heaterActive;  // Burner currently firing (0x20 in status byte)

    // Pump (from A5 ACTION_PUMP_STATUS)
    bool      pumpRunning;
    uint16_t  pumpWatts;
    uint16_t  pumpRPM;
    PumpSpeed pumpSpeedPreset;  // Nearest named preset derived from RPM

    // Chlorinator (from IntelliChlor frames)
    uint8_t  chlorOutput;    // Output % (0-100)
    uint16_t chlorSaltPPM;   // Salt level in PPM (raw byte × 50)
    bool     chlorActive;
    bool     chlorLowSalt;
    bool     chlorInspectCell;

    // Timestamps (millis()) for detecting stale data / last-seen reporting
    unsigned long lastUpdate;
    unsigned long lastPumpUpdate;
    unsigned long lastChlorUpdate;
    unsigned long lastRs485Activity;
};

// ============================================================
// Protocol functions
// ============================================================
namespace Pentair {

    // --- A5 protocol ---

    // 16-bit sum of len bytes starting at data[0]
    uint16_t calcChecksum(const uint8_t* data, size_t len);

    // Scan buf for the next valid A5 packet; set bytesConsumed to advance the ring buffer
    bool parsePacket(const uint8_t* buf, size_t len,
                     PentairPacket& packet, size_t& bytesConsumed);

    // Serialize an A5 packet into buf; returns total bytes written (0 on overflow)
    size_t buildPacket(uint8_t* buf, size_t bufSize,
                       uint8_t dst, uint8_t src, uint8_t action,
                       const uint8_t* data, uint8_t dataLen);

    // Update PoolState from a received A5 packet
    void processPacket(const PentairPacket& packet, PoolState& state);

    size_t buildCircuitCommand(uint8_t* buf, size_t bufSize,
                               uint8_t src, uint8_t dst,
                               uint8_t circuit, bool on);

    size_t buildHeatModeCommand(uint8_t* buf, size_t bufSize,
                                uint8_t src, uint8_t dst,
                                bool heaterOn);

    size_t buildHeatSetpointCommand(uint8_t* buf, size_t bufSize,
                                    uint8_t src, uint8_t dst,
                                    uint8_t poolSetpoint);

    // Step 1/3 of pump speed change: grant (true) or release (false) remote control
    size_t buildPumpRemoteControl(uint8_t* buf, size_t bufSize,
                                  uint8_t src, bool remote);

    // Step 2/3 of pump speed change: write program register 0x0321 with program value
    size_t buildPumpProgramCommand(uint8_t* buf, size_t bufSize,
                                   uint8_t src, uint8_t program);

    // --- IntelliChlor protocol ([10 02] ... [checksum] [10 03]) ---

    // (18 + sum of data bytes) & 0xFF — the constant 18 is part of the spec
    uint8_t calcChlorChecksum(const uint8_t* data, size_t len);

    // Scan buf for the next valid IntelliChlor frame; set bytesConsumed to advance the ring buffer
    bool parseChlorPacket(const uint8_t* buf, size_t len,
                          ChlorPacket& packet, size_t& bytesConsumed);

    // Update PoolState from a received IntelliChlor packet
    void processChlorPacket(const ChlorPacket& packet, PoolState& state);

    // Build a status query to the chlorinator (CHLOR_GET_STATUS)
    size_t buildChlorQuery(uint8_t* buf, size_t bufSize);

    // Build a set-output command (0-100 %)
    size_t buildChlorSetOutput(uint8_t* buf, size_t bufSize, uint8_t percent);
}
