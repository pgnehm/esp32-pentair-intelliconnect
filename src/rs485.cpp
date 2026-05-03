/*
 * rs485.cpp
 *
 * Half-duplex RS485 driver for the Pentair bus.
 *
 * The MAX485 transceiver shares DE and RE on a single GPIO (GPIO16). Asserting
 * the pin HIGH enables the transmit driver and disables the receive comparator;
 * pulling it LOW does the opposite. The bus must be returned to receive mode
 * immediately after each transmission, because all devices — including the
 * IntelliConnect controller — can transmit at any time.
 *
 * loop() drains the UART FIFO, strips unrecognized leading bytes, and dispatches
 * to the appropriate parser based on the first byte of the buffer:
 *   0xFF → A5 parser (controller, pump, heater)
 *   0x10 → IntelliChlor parser (salt chlorinator)
 */

#include "rs485.h"
#include "config.h"
#include "udp_log.h"

// ============================================================
// Construction / initialization
// ============================================================

RS485::RS485(uint8_t rxPin, uint8_t txPin, uint8_t deRePin, uint8_t uartNum)
    : _deRePin(deRePin)
    , _rxBufLen(0)
    , _packetReady(false)
    , _chlorPacketReady(false)
    , _bytesReceived(0)
    , _packetsReceived(0)
    , _lastActivityMillis(0)
{
    _serial = new HardwareSerial(uartNum);
    // RS485_RX_PIN / RS485_TX_PIN from config.h are passed directly to
    // _serial->begin() in RS485::begin(), so the constructor pin parameters
    // are redundant. They are accepted for API clarity (the caller can see all
    // four hardware assignments in one place) but suppressed here to avoid
    // unused-parameter compiler warnings.
    (void)rxPin;
    (void)txPin;
    _lastPacket      = {};
    _lastChlorPacket = {};
}

void RS485::begin(unsigned long baud) {
    pinMode(_deRePin, OUTPUT);
    setReceiveMode();  // Default to listening before anything is on the bus
    _serial->setRxBufferSize(512);
    _serial->begin(baud, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    Serial.printf("[RS485] Initialized: baud=%lu, RX=%d, TX=%d, DE/RE=%d\n",
                  baud, RS485_RX_PIN, RS485_TX_PIN, _deRePin);
}

// ============================================================
// Main receive loop
// ============================================================

void RS485::loop() {
    // Rolling capture buffer for raw hex UdpLog dump (diagnostic only)
    static uint8_t       capBuf[64];
    static size_t        capLen = 0;
    static unsigned long lastCapDump = 0;

    // Drain the UART hardware FIFO into the ring buffer
    while (_serial->available() && _rxBufLen < RX_BUF_SIZE) {
        uint8_t b = _serial->read();
        _rxBuf[_rxBufLen++] = b;
        _bytesReceived++;
        _lastActivityMillis = millis();
        if (capLen < sizeof(capBuf)) capBuf[capLen++] = b;
    }

    // Emit a raw hex dump every 2 seconds for bus traffic inspection
    if (millis() - lastCapDump > 2000 && capLen > 0) {
        lastCapDump = millis();
        char hexbuf[220];
        int n = snprintf(hexbuf, sizeof(hexbuf), "[RAW] %u bytes |", (unsigned)capLen);
        for (size_t k = 0; k < capLen && n < 210; k++)
            n += snprintf(hexbuf+n, sizeof(hexbuf)-n, " %02X", capBuf[k]);
        UdpLog.println(hexbuf);
        capLen = 0;
    }

    if (_rxBufLen == 0) return;

    // Strip leading bytes that cannot start a known packet format.
    // A5 frames begin with 0xFF; IntelliChlor frames begin with 0x10.
    // Anything else is bus noise, line turnaround echo, or mid-stream sync loss.
    {
        size_t skip = 0;
        while (skip < _rxBufLen && _rxBuf[skip] != 0xFF && _rxBuf[skip] != 0x10) skip++;
        // Retain the last byte even if unrecognized — it may be the first byte of a
        // preamble whose second byte has not arrived yet.
        if (skip == _rxBufLen && _rxBufLen > 0) skip = _rxBufLen - 1;
        if (skip > 0) {
            memmove(_rxBuf, &_rxBuf[skip], _rxBufLen - skip);
            _rxBufLen -= skip;
        }
    }

    if (_rxBufLen < PENTAIR_MIN_PACKET_SIZE) return;

    size_t consumed = 0;

    if (_rxBuf[0] == 0xFF) {
        // A5 protocol — preamble is FF 00 FF A5
        PentairPacket a5pkt;
        if (Pentair::parsePacket(_rxBuf, _rxBufLen, a5pkt, consumed)) {
            _lastPacket  = a5pkt;
            _packetReady = true;
            _packetsReceived++;
            UdpLog.printf("[PARSE] src=%02X dst=%02X act=%02X len=%d\n",
                          a5pkt.src, a5pkt.dst, a5pkt.action, a5pkt.dataLen);
        }
    } else {
        // IntelliChlor protocol — frame starts with 10 02, ends with 10 03
        ChlorPacket chlorpkt;
        if (Pentair::parseChlorPacket(_rxBuf, _rxBufLen, chlorpkt, consumed)) {
            _lastChlorPacket  = chlorpkt;
            _chlorPacketReady = true;
            _packetsReceived++;
        }
    }

    // Slide consumed bytes off the front of the ring buffer
    if (consumed > 0 && consumed <= _rxBufLen) {
        memmove(_rxBuf, &_rxBuf[consumed], _rxBufLen - consumed);
        _rxBufLen -= consumed;
    }

    // Safety valve: if the buffer is almost full and no parser made progress,
    // the bus is producing continuous noise or we lost sync badly. Discard the
    // entire buffer so we don't stall permanently. This sacrifices at most one
    // packet but recovers automatically on the next valid preamble.
    if (_rxBufLen >= RX_BUF_SIZE - 32) _rxBufLen = 0;
}

// ============================================================
// Transmit
// ============================================================

void RS485::sendPacket(const uint8_t* data, size_t len) {
    setTransmitMode();
    // RS485_TX_DELAY_US: give the MAX485 DE/RE propagation time before the first bit
    delayMicroseconds(RS485_TX_DELAY_US);
    _serial->write(data, len);
    _serial->flush();  // Block until shift register is empty
    // RS485_TX_COMPLETE_WAIT_MS: hold DE high until the last bit has propagated off the bus
    delay(RS485_TX_COMPLETE_WAIT_MS);
    setReceiveMode();
}

// ============================================================
// Packet accessors — clear ready flag on read (single-consumer)
// ============================================================

PentairPacket RS485::getPacket() {
    _packetReady = false;
    return _lastPacket;
}

ChlorPacket RS485::getChlorPacket() {
    _chlorPacketReady = false;
    return _lastChlorPacket;
}

// ============================================================
// Direction control
// ============================================================

void RS485::setReceiveMode() {
    digitalWrite(_deRePin, LOW);   // DE=0 disables driver, RE=0 enables receiver
}

void RS485::setTransmitMode() {
    digitalWrite(_deRePin, HIGH);  // DE=1 enables driver, RE=1 disables receiver
}
