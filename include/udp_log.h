/*
 * udp_log.h
 *
 * Lightweight UDP broadcast logger for remote debugging without a serial cable.
 *
 * All messages are sent to the LAN broadcast address (255.255.255.255) on
 * UDP_LOG_PORT (4210). Any PC on the same subnet can receive them with:
 *
 *   Windows: Start-UdpClient -Port 4210 (PowerShell) or nc -u -l 4210 (WSL)
 *   Linux/macOS: nc -u -l 4210
 *   Python: see project DOCUMENTATION.md for a one-liner snippet
 *
 * Messages are also echoed to USB Serial so no information is lost when
 * a serial monitor is attached.
 *
 * The global UdpLog instance is defined in udp_log.cpp and declared here
 * for use by all translation units.
 */

#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>
#include "config.h"

class UdpLogger {
public:
    // Open the UDP socket. Call once from setup(), after WiFi is connected.
    void begin();

    // printf-style formatted log. Message is truncated at 255 chars.
    void printf(const char* fmt, ...);

    // Print a plain string followed by a newline.
    void println(const char* msg = "");

private:
    WiFiUDP _udp;
    // Broadcast msg to UDP_LOG_PORT and mirror to Serial.
    void _send(const char* msg);
};

// Global singleton — include this header and call UdpLog.printf(...)
extern UdpLogger UdpLog;
