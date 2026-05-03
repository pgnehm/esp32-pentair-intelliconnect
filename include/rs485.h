/*
 * rs485.h
 *
 * RS485 transceiver driver for the Pentair bus.
 *
 * The MAX485 chip is half-duplex: DE/RE pin HIGH = transmit, LOW = receive.
 * Both protocols share a single physical bus, so loop() dispatches incoming
 * bytes to the appropriate parser based on the leading byte (0xFF = A5,
 * 0x10 = IntelliChlor).
 *
 * Hardware: UART1 on GPIO17(TX) / GPIO18(RX) / GPIO16(DE/RE).
 */

#pragma once

#include <Arduino.h>
#include "pentair_protocol.h"

class RS485 {
public:
    RS485(uint8_t rxPin, uint8_t txPin, uint8_t deRePin, uint8_t uartNum);

    void begin(unsigned long baud);

    // Call every loop() iteration: drains UART FIFO, runs parsers, emits UdpLog
    void loop();

    // Asserts DE/RE, writes data, waits for flush, then returns to receive mode
    void sendPacket(const uint8_t* data, size_t len);

    // A5 packet availability (pump, controller, heater traffic)
    bool hasPacket() const { return _packetReady; }
    // Clears the ready flag and returns the last parsed A5 packet
    PentairPacket getPacket();

    // IntelliChlor packet availability ([10 02 ... 10 03] frames)
    bool hasChlorPacket() const { return _chlorPacketReady; }
    // Clears the ready flag and returns the last parsed IntelliChlor packet
    ChlorPacket getChlorPacket();

    unsigned long getBytesReceived()      const { return _bytesReceived; }
    unsigned long getPacketsReceived()    const { return _packetsReceived; }
    unsigned long getLastActivityMillis() const { return _lastActivityMillis; }

private:
    // Drive DE/RE pin to select bus direction
    void setReceiveMode();   // DE/RE LOW  — listen
    void setTransmitMode();  // DE/RE HIGH — drive

    HardwareSerial* _serial;
    uint8_t _deRePin;

    static const size_t RX_BUF_SIZE = 256;
    uint8_t _rxBuf[RX_BUF_SIZE];
    size_t  _rxBufLen;

    PentairPacket _lastPacket;
    bool          _packetReady;

    ChlorPacket   _lastChlorPacket;
    bool          _chlorPacketReady;

    unsigned long _bytesReceived;
    unsigned long _packetsReceived;
    unsigned long _lastActivityMillis;
};
