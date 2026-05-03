/*
 * udp_log.cpp
 *
 * Implementation of the UdpLogger broadcast logger.
 *
 * All log output is mirrored to both USB Serial (for bench use) and to the
 * LAN broadcast address so a PC can sniff messages without a physical cable.
 * Broadcasting avoids needing to know the PC's IP address at compile time;
 * any host running a UDP listener on UDP_LOG_PORT will receive the traffic.
 */

#include "udp_log.h"

// Define the global singleton instance that all modules reference via UdpLog
UdpLogger UdpLog;

void UdpLogger::begin() {
    // Bind to the log port so the UDP socket is ready to send.
    // We send only (never receive on this socket), but begin() is required
    // before write calls will succeed.
    _udp.begin(UDP_LOG_PORT);
}

void UdpLogger::printf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    // vsnprintf guarantees null-termination and never overruns buf
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Mirror to Serial first so a crash during _send doesn't lose the message
    Serial.print(buf);
    _send(buf);
}

void UdpLogger::println(const char* msg) {
    Serial.println(msg);
    // Append newline so the UDP receiver can parse line-by-line
    char buf[256];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    _send(buf);
}

void UdpLogger::_send(const char* msg) {
    // Broadcast to 255.255.255.255 — reaches every host on the local subnet
    // without requiring knowledge of any specific PC's IP address.
    _udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_LOG_PORT);
    _udp.write((const uint8_t*)msg, strlen(msg));
    _udp.endPacket();
}
