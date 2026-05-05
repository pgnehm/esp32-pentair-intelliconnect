/*
 * pentair_protocol.cpp
 *
 * Parsers, builders, and state processors for the two Pentair bus protocols:
 *   - A5: used by the IntelliConnect controller, IntelliFlo pump, and gas heater
 *   - IntelliChlor: used by the salt chlorinator (address 0x50)
 *
 * Both parsers are designed for streaming use: they accept a raw byte buffer
 * and return the number of bytes consumed so the caller can slide a ring buffer
 * forward, handling partial packets and multi-packet bursts correctly.
 */

#include "pentair_protocol.h"
#include "config.h"

namespace Pentair {

// ============================================================
// A5 checksum
// ============================================================

uint16_t calcChecksum(const uint8_t* data, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

// ============================================================
// A5 packet parser
// ============================================================

bool parsePacket(const uint8_t* buf, size_t len, PentairPacket& packet, size_t& bytesConsumed) {
    packet.valid = false;
    bytesConsumed = 0;

    for (size_t i = 0; i + PENTAIR_MIN_PACKET_SIZE <= len; i++) {
        // Scan for the 3-byte sync preamble: FF 00 FF
        if (buf[i] != 0xFF) continue;
        if (i + 1 >= len || buf[i + 1] != 0x00) continue;
        if (i + 2 >= len || buf[i + 2] != 0xFF) continue;
        // Fourth byte must be A5 — the start of the header and checksum coverage
        if (i + 3 >= len || buf[i + 3] != PENTAIR_HEADER) continue;

        size_t hdrStart = i + 3;  // Points to A5 byte (checksum starts here)

        if (hdrStart + 6 > len) {
            // Header bytes haven't arrived yet; hold position and wait
            bytesConsumed = i;
            return false;
        }

        // A5 header layout: [A5][proto][dst][src][action][len]
        uint8_t protocol = buf[hdrStart + 1];
        uint8_t dst      = buf[hdrStart + 2];
        uint8_t src      = buf[hdrStart + 3];
        uint8_t action   = buf[hdrStart + 4];
        uint8_t dataLen  = buf[hdrStart + 5];

        if (dataLen > PENTAIR_MAX_DATA_LEN) {
            // Implausibly large data field — this isn't a real packet start
            bytesConsumed = i + 4;
            continue;
        }

        size_t packetEnd = hdrStart + 6 + dataLen + 2;  // +2 for 16-bit checksum
        if (packetEnd > len) {
            // Data + checksum bytes still in flight; hold position
            bytesConsumed = i;
            return false;
        }

        // Checksum covers from the A5 byte through the last data byte
        size_t checksumDataLen = 6 + dataLen;
        uint16_t expectedChecksum = calcChecksum(&buf[hdrStart], checksumDataLen);
        uint16_t receivedChecksum = (buf[hdrStart + checksumDataLen] << 8) | buf[hdrStart + checksumDataLen + 1];

        if (expectedChecksum != receivedChecksum) {
            // Bad checksum — skip past the preamble and try the next candidate
            bytesConsumed = i + 4;
            continue;
        }

        packet.protocol = protocol;
        packet.dst      = dst;
        packet.src      = src;
        packet.action   = action;
        packet.dataLen  = dataLen;
        memcpy(packet.data, &buf[hdrStart + 6], dataLen);
        packet.checksum = receivedChecksum;
        packet.valid    = true;

        bytesConsumed = packetEnd;
        return true;
    }

    // No valid packet found; safe to discard everything except the last 3 bytes,
    // which might be the leading bytes of a preamble split across two reads.
    bytesConsumed = (len > 3) ? len - 3 : 0;
    return false;
}

// ============================================================
// A5 packet builder
// ============================================================

size_t buildPacket(uint8_t* buf, size_t bufSize,
                   uint8_t dst, uint8_t src, uint8_t action,
                   const uint8_t* data, uint8_t dataLen) {
    // Frame: [FF 00 FF] [A5] [00] [dst] [src] [action] [dataLen] [data...] [ckH] [ckL]
    size_t totalLen = 3 + 1 + 1 + 1 + 1 + 1 + 1 + dataLen + 2;
    if (totalLen > bufSize) return 0;

    size_t pos = 0;
    buf[pos++] = 0xFF;
    buf[pos++] = 0x00;
    buf[pos++] = 0xFF;
    buf[pos++] = PENTAIR_HEADER;  // 0xA5 — checksum coverage begins here
    buf[pos++] = 0x00;            // protocol field; always 0x00 on this bus
    buf[pos++] = dst;
    buf[pos++] = src;
    buf[pos++] = action;
    buf[pos++] = dataLen;

    for (uint8_t i = 0; i < dataLen; i++) buf[pos++] = data[i];

    // Checksum starts at buf[3] (the A5 byte), covering header + data
    uint16_t checksum = calcChecksum(&buf[3], 6 + dataLen);
    buf[pos++] = (checksum >> 8) & 0xFF;
    buf[pos++] = checksum & 0xFF;

    return pos;
}

// ============================================================
// A5 state processor
// ============================================================

void processPacket(const PentairPacket& packet, PoolState& state) {
    switch (packet.action) {
        case ACTION_STATUS_RESPONSE: {
            if (packet.dataLen < 20) break;

            // data[2-3]: 16-bit circuit bitmask, confirmed via nodejs-poolController EQUIP1/EQUIP2
            uint16_t circuitState = (packet.data[2] << 8) | packet.data[3];
            state.poolOn      = circuitState & (1 << (CIRCUIT_POOL - 1));
            state.poolLightOn = circuitState & (1 << (CIRCUIT_POOL_LIGHT - 1));
            state.cleanerOn   = circuitState & (1 << (CIRCUIT_CLEANER - 1));

            // data[14]: controller's water temperature reading. Intentionally NOT written to
            // state.waterTemp — the IntelliChlor CHLOR_EXTENDED_STATUS (0x16) provides a more
            // accurate reading from the cell's own thermistor and takes precedence.

            if (packet.dataLen > 16) {
                // data[16]: 0x00=heater off, 0x20=heater actively firing
                state.heaterActive = (packet.data[16] != 0);
            }
            if (packet.dataLen > 18) {
                state.airTemp = packet.data[18];
            }
            if (packet.dataLen > 22) {
                // Lower 2 bits encode pool heat mode (see HeatMode enum)
                state.heaterOn = (packet.data[22] & 0x03) != HEAT_OFF;
            }

            state.lastUpdate = millis();
            break;
        }

        case ACTION_HEAT_STATUS: {
            // Action 0x08 carries the configured temperature setpoints
            if (packet.dataLen < 4) break;
            state.poolSetpoint = packet.data[0];
            break;
        }

        case ACTION_PUMP_STATUS: {
            if (packet.dataLen < 8) break;
            state.pumpWatts   = (packet.data[3] << 8) | packet.data[4];
            state.pumpRPM     = (packet.data[5] << 8) | packet.data[6];
            state.pumpRunning = state.pumpRPM > 0;
            // IntelliConnect does not broadcast pool-on state over RS485, so
            // infer it from pump RPM — if the pump is running, the pool is on.
            state.poolOn      = state.pumpRunning;

            // Map live RPM to the nearest named speed preset for MQTT reporting.
            // Boundaries sit at midpoints between adjacent preset RPMs:
            //   Standard(1650) | 1660 | Test(1670) | 1985 | Heat(2300) | 2350 | Cleaner(2400)
            if      (state.pumpRPM == 0)    state.pumpSpeedPreset = PUMP_SPEED_STOP;
            else if (state.pumpRPM <= 1660) state.pumpSpeedPreset = PUMP_SPEED_STANDARD;
            else if (state.pumpRPM <= 1985) state.pumpSpeedPreset = PUMP_SPEED_TEST;
            else if (state.pumpRPM <= 2350) state.pumpSpeedPreset = PUMP_SPEED_HEAT;
            else                            state.pumpSpeedPreset = PUMP_SPEED_CLEANER;

            state.lastPumpUpdate = millis();
            break;
        }

        default:
            break;
    }
}

// ============================================================
// A5 command builders
// ============================================================

size_t buildCircuitCommand(uint8_t* buf, size_t bufSize,
                           uint8_t src, uint8_t dst,
                           uint8_t circuit, bool on) {
    // Data: [circuit_number][0x01=on / 0x00=off]
    // NOTE: IntelliConnect (0x10) silently ignores ACTION_SET_CIRCUIT (0x86)
    // from external RS485 devices. This function is provided for completeness
    // and may work on other Pentair controllers (e.g. IntelliTouch, EasyTouch).
    uint8_t data[] = { circuit, on ? (uint8_t)1 : (uint8_t)0 };
    return buildPacket(buf, bufSize, dst, src, ACTION_SET_CIRCUIT, data, sizeof(data));
}

size_t buildHeatModeCommand(uint8_t* buf, size_t bufSize,
                            uint8_t src, uint8_t dst,
                            bool heaterOn) {
    // Heat mode byte encoding: bits [1:0] = pool heat mode, bits [3:2] = spa heat mode.
    // This installation has no spa, so spa bits are always 0.
    // See HeatMode enum: 0=off, 1=gas heater, 2=solar preferred, 3=solar only.
    // NOTE: IntelliConnect ignores ACTION_SET_HEAT_MODE from external RS485 devices.
    uint8_t data[] = { heaterOn ? (uint8_t)HEAT_HEATER : (uint8_t)HEAT_OFF };
    return buildPacket(buf, bufSize, dst, src, ACTION_SET_HEAT_MODE, data, sizeof(data));
}

size_t buildHeatSetpointCommand(uint8_t* buf, size_t bufSize,
                                uint8_t src, uint8_t dst,
                                uint8_t poolSetpoint) {
    // The protocol packs pool and spa setpoints into a single packet.
    // This installation has no spa; 0x00 is sent as the spa setpoint.
    // NOTE: IntelliConnect ignores this command from external RS485 devices.
    uint8_t data[] = { poolSetpoint, 0x00 };
    return buildPacket(buf, bufSize, dst, src, ACTION_SET_HEAT_MODE, data, sizeof(data));
}

size_t buildPumpRemoteControl(uint8_t* buf, size_t bufSize, uint8_t src, bool remote) {
    // ACTION_PUMP_REMOTE (0x04): 0xFF grants remote control, 0x00 releases it back to local
    uint8_t data[] = { remote ? (uint8_t)0xFF : (uint8_t)0x00 };
    return buildPacket(buf, bufSize, ADDR_PUMP, src, ACTION_PUMP_REMOTE, data, sizeof(data));
}

size_t buildPumpProgramCommand(uint8_t* buf, size_t bufSize, uint8_t src, uint8_t program) {
    // ACTION_PUMP_WRITE (0x01): select a speed program on pump register 0x0321.
    // Only slot values are accepted: PUMP_PROG_STOP(0x00), and 0x08/0x10/0x18/0x20
    // for programs 1-4. The RPM for each slot is stored in the pump's own memory.
    uint8_t data[] = { 0x03, 0x21, 0x00, program };
    return buildPacket(buf, bufSize, ADDR_PUMP, src, ACTION_PUMP_WRITE, data, sizeof(data));
}

// ============================================================
// IntelliChlor protocol ([10 02] ... [checksum] [10 03])
// ============================================================

uint8_t calcChlorChecksum(const uint8_t* data, size_t len) {
    // Chlorinator checksum = (18 + sum of payload bytes) mod 256.
    // The constant 18 (0x12) is part of the IntelliChlor spec — it accounts for
    // the fixed overhead bytes [10 02] in a way that makes the math work out.
    uint16_t sum = 18;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum & 0xFF;
}

bool parseChlorPacket(const uint8_t* buf, size_t len,
                      ChlorPacket& packet, size_t& bytesConsumed) {
    packet.valid = false;
    bytesConsumed = 0;

    for (size_t i = 0; i + 5 <= len; i++) {
        if (buf[i] != 0x10 || buf[i+1] != 0x02) continue;

        // Walk forward from the payload start to find the end-of-frame marker [10 03].
        // The byte immediately before [10 03] is the checksum.
        size_t dataStart = i + 2;
        size_t j = dataStart;
        while (j + 2 < len && !(buf[j+1] == 0x10 && buf[j+2] == 0x03)) j++;

        if (j + 2 >= len) {
            // End-of-frame not yet received; hold position and wait for more data
            bytesConsumed = i;
            return false;
        }

        // j = checksum byte, j+1 = 0x10, j+2 = 0x03
        size_t dataLen = j - dataStart;
        if (dataLen < 2 || dataLen > sizeof(packet.data) + 1) {
            bytesConsumed = i + 2;
            continue;
        }

        uint8_t expectedCk = calcChlorChecksum(&buf[dataStart], dataLen);
        if (expectedCk != buf[j]) {
            bytesConsumed = i + 2;
            continue;
        }

        // Payload layout inside the frame: [dst][action][data...]
        packet.dst     = buf[dataStart];
        packet.action  = buf[dataStart + 1];
        packet.dataLen = dataLen - 2;  // subtract dst and action bytes
        memcpy(packet.data, &buf[dataStart + 2], packet.dataLen);
        packet.valid   = true;
        bytesConsumed  = j + 3;  // advance past checksum + [10 03]
        return true;
    }

    // Keep the last byte in case it is the leading 0x10 of the next preamble
    bytesConsumed = (len > 1) ? len - 1 : 0;
    return false;
}

// ============================================================
// IntelliChlor state processor
// ============================================================

void processChlorPacket(const ChlorPacket& packet, PoolState& state) {
    // CHLOR_STATUS_RESP (0x01) and CHLOR_OUTPUT_RESP (0x12) data layout:
    //   byte 0: salt_raw  — PPM = salt_raw × 50  (e.g. 78 → 3900 PPM)
    //   byte 1: flags     — bit 7 (0x80) = low salt; bit 2 (0x04) = inspect cell
    //   byte 2: output %  — 0–100 (current chlorine generation rate)
    if (packet.action == CHLOR_OUTPUT_RESP || packet.action == CHLOR_STATUS_RESP) {
        if (packet.dataLen < 1) return;
        state.chlorSaltPPM = (uint16_t)packet.data[0] * 50;
        if (packet.dataLen >= 2) {
            state.chlorLowSalt     = (packet.data[1] & 0x80) != 0;
            state.chlorInspectCell = (packet.data[1] & 0x04) != 0;
        }
        if (packet.dataLen >= 3) {
            state.chlorOutput = packet.data[2];
            state.chlorActive = (packet.data[2] > 0);
        }
        state.lastChlorUpdate = millis();
    }

    // CHLOR_EXTENDED_STATUS (0x16) data layout:
    //   byte 0: water temp °F (from chlorinator cell thermistor — most accurate source)
    //   byte 1: chlorine output % (0–100)
    //   byte 2+: additional status flags
    if (packet.action == CHLOR_EXTENDED_STATUS) {
        if (packet.dataLen >= 1) {
            uint8_t t = packet.data[0];
            if (t > 32 && t < 110) state.waterTemp = t;
        }
        if (packet.dataLen >= 2) {
            state.chlorOutput = packet.data[1];
            state.chlorActive = (packet.data[1] > 0);
        }
        state.lastChlorUpdate = millis();
    }
}

// ============================================================
// IntelliChlor packet builders
// ============================================================

size_t buildChlorQuery(uint8_t* buf, size_t bufSize) {
    // Frame: [10 02] [0x50] [CHLOR_GET_STATUS] [checksum] [10 03]
    if (bufSize < 7) return 0;
    uint8_t data[] = { 0x50, CHLOR_GET_STATUS };
    uint8_t ck = calcChlorChecksum(data, sizeof(data));
    buf[0] = 0x10; buf[1] = 0x02;
    buf[2] = 0x50; buf[3] = CHLOR_GET_STATUS;
    buf[4] = ck;
    buf[5] = 0x10; buf[6] = 0x03;
    return 7;
}

size_t buildChlorSetOutput(uint8_t* buf, size_t bufSize, uint8_t percent) {
    // Frame: [10 02] [0x50] [CHLOR_SET_OUTPUT] [percent] [checksum] [10 03]
    if (bufSize < 8) return 0;
    if (percent > 100) percent = 100;
    uint8_t data[] = { 0x50, CHLOR_SET_OUTPUT, percent };
    uint8_t ck = calcChlorChecksum(data, sizeof(data));
    buf[0] = 0x10; buf[1] = 0x02;
    buf[2] = 0x50; buf[3] = CHLOR_SET_OUTPUT;
    buf[4] = percent;
    buf[5] = ck;
    buf[6] = 0x10; buf[7] = 0x03;
    return 8;
}

} // namespace Pentair
