/*
 * main.cpp
 *
 * Application entry point for the ESP32 Pentair pool controller.
 *
 * This firmware passively listens to the Pentair RS485 bus, parses A5 and
 * IntelliChlor frames, and publishes equipment state to Home Assistant via MQTT.
 * It can also send pump speed and chlorinator output commands.
 *
 * IntelliConnect (0x10) is the pool controller; it does NOT accept RS485 SET
 * commands from external devices. Pool on/off is inferred from pump RPM, and
 * water temperature comes from the IntelliChlor CHLOR_EXTENDED_STATUS packet.
 *
 * OTA target: 192.168.86.118 (see config.h)
 * UDP debug log: broadcast to port 4210
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <time.h>
#include "config.h"
#include "udp_log.h"
#include "rs485.h"
#include "mqtt_handler.h"
#include "pentair_protocol.h"

// ============================================================
// Globals
// ============================================================

RS485        rs485(RS485_RX_PIN, RS485_TX_PIN, RS485_DE_RE_PIN, RS485_UART_NUM);
WiFiClient   wifiClient;
MqttHandler  mqtt(wifiClient);
PoolState    poolState = {};

// Timestamps for periodic tasks
unsigned long lastChlorPoll    = 0;
unsigned long lastHeartbeat    = 0;
unsigned long lastPeriodicPublish = 0;

// ============================================================
// WiFi helpers
// ============================================================

void setupWiFi() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println("\n[WiFi] Connection timeout! Check SSID/password in config.h");
            return;
        }
        delay(500);
        Serial.print(".");
    }

    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

// ============================================================
// MQTT command handler
// ============================================================

void handleMqttCommand(const char* topic, const char* payload) {
    uint8_t txBuf[32];
    size_t  len = 0;
    String  t(topic);

    UdpLog.printf("[CMD] %s = %s\n", topic, payload);

    if (t.endsWith("/restart")) {
        UdpLog.printf("[CMD] Restarting ESP32...\n");
        delay(100);
        ESP.restart();
        return;
    }
    else if (t.endsWith("/pump/speed/set")) {
        // Pump speed change requires a 3-step sequence to avoid the pump
        // ignoring the command: (1) take remote control, (2) write the
        // program register, (3) release remote control back to local.
        uint8_t prog = PUMP_PROG_1;  // Default to Low if payload is unrecognized
        if      (strcmp(payload, "Normal") == 0) prog = PUMP_PROG_2;
        else if (strcmp(payload, "High")   == 0) prog = PUMP_PROG_3;
        else if (strcmp(payload, "Max")    == 0) prog = PUMP_PROG_4;
        else if (strcmp(payload, "Stop")   == 0) prog = PUMP_PROG_STOP;

        uint8_t step1[16], step2[16], step3[16];
        size_t l1 = Pentair::buildPumpRemoteControl(step1, sizeof(step1), ADDR_THIS_DEVICE, true);
        size_t l2 = Pentair::buildPumpProgramCommand(step2, sizeof(step2), ADDR_THIS_DEVICE, prog);
        size_t l3 = Pentair::buildPumpRemoteControl(step3, sizeof(step3), ADDR_THIS_DEVICE, false);

        if (l1 && l2 && l3) {
            rs485.sendPacket(step1, l1);
            delay(100);  // Brief pause between commands — pump needs time to process each step
            rs485.sendPacket(step2, l2);
            delay(100);
            rs485.sendPacket(step3, l3);
            UdpLog.printf("[CMD] Pump speed set to %s\n", payload);
        }
        return;
    }
    else if (t.endsWith("/chlorinator/set")) {
        uint8_t percent = atoi(payload);
        len = Pentair::buildChlorSetOutput(txBuf, sizeof(txBuf), percent);
    }

    if (len > 0) {
        rs485.sendPacket(txBuf, len);
        UdpLog.printf("[CMD] Sent %d bytes on RS485 bus\n", len);
    }
}

// ============================================================
// Debug helpers
// ============================================================

void printPacketDebug(const PentairPacket& pkt) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "[PKT] src=0x%02X dst=0x%02X action=0x%02X len=%d |",
                     pkt.src, pkt.dst, pkt.action, pkt.dataLen);
    for (int i = 0; i < pkt.dataLen && i < 16 && n < (int)sizeof(buf)-4; i++)
        n += snprintf(buf+n, sizeof(buf)-n, " %02X", pkt.data[i]);
    if (pkt.dataLen > 16) strncat(buf, " ...", sizeof(buf)-n-1);
    UdpLog.println(buf);
}

// ============================================================
// Setup
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(2000);  // Let USB CDC enumerate before printing
    Serial.println("\n========================================");
    Serial.println("  ESP32 Pentair Pool Controller");
    Serial.println("========================================\n");

    rs485.begin(PENTAIR_BAUD_RATE);
    setupWiFi();
    UdpLog.begin();

    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { Serial.println("[OTA] Starting update..."); });
    ArduinoOTA.onEnd([]()   { Serial.println("\n[OTA] Done. Rebooting."); });
    ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[OTA] Error %u\n", error); });
    ArduinoOTA.begin();
    Serial.println("[OTA] Ready");

    configTime(NTP_UTC_OFFSET_SEC, NTP_DST_OFFSET_SEC, NTP_SERVER);
    Serial.println("[NTP] Time sync started");

    mqtt.begin(MQTT_SERVER, MQTT_PORT, MQTT_USER, MQTT_PASSWORD, MQTT_CLIENT_ID);
    mqtt.onCommand(handleMqttCommand);

    Serial.println("[MAIN] Setup complete. Listening on RS485 bus...\n");
}

// ============================================================
// Main loop
// ============================================================

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Disconnected, reconnecting...");
        setupWiFi();
    }

    ArduinoOTA.handle();
    mqtt.loop();
    rs485.loop();

    // Process any complete A5 frame (controller status, pump status, heat status)
    if (rs485.hasPacket()) {
        PentairPacket pkt = rs485.getPacket();
        printPacketDebug(pkt);
        Pentair::processPacket(pkt, poolState);
        mqtt.publishState(poolState);
    }

    // Process any complete IntelliChlor frame (salt PPM, output %, water temp)
    if (rs485.hasChlorPacket()) {
        ChlorPacket cpkt = rs485.getChlorPacket();
        char cbuf[128];
        int cn = snprintf(cbuf, sizeof(cbuf), "[CHLOR] action=0x%02X len=%d |", cpkt.action, cpkt.dataLen);
        for (int i = 0; i < cpkt.dataLen && cn < (int)sizeof(cbuf)-4; i++)
            cn += snprintf(cbuf+cn, sizeof(cbuf)-cn, " %02X", cpkt.data[i]);
        UdpLog.println(cbuf);
        Pentair::processChlorPacket(cpkt, poolState);
        mqtt.publishState(poolState);
    }

    // Sync last-activity timestamp every loop so the rs485_alive health sensor
    // reflects the most recent byte seen on the bus, not just packet completions.
    poolState.lastRs485Activity = rs485.getLastActivityMillis();

    // Refresh timestamps in HA every 10 seconds even when no new packets arrive
    if (millis() - lastPeriodicPublish > 10000) {
        lastPeriodicPublish = millis();
        mqtt.publishState(poolState);
    }

    // Actively query the chlorinator every 30 seconds.
    // The IntelliConnect polls it too, but querying ourselves ensures we get
    // fresh salt PPM and water temp even when controller traffic is sparse.
    if (millis() - lastChlorPoll > 30000) {
        lastChlorPoll = millis();
        uint8_t txBuf[8];
        size_t len = Pentair::buildChlorQuery(txBuf, sizeof(txBuf));
        if (len > 0) rs485.sendPacket(txBuf, len);
    }

    // Periodic heartbeat: log uptime and connectivity status for remote monitoring
    if (millis() - lastHeartbeat > 5000) {
        lastHeartbeat = millis();
        char timebuf[24] = "??:??:??";
        struct tm ti;
        if (getLocalTime(&ti, 0))
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &ti);
        UdpLog.printf("[ALIVE] %s uptime=%lus ip=%s mqtt=%s rx_bytes=%lu\n",
            timebuf,
            millis() / 1000,
            WiFi.localIP().toString().c_str(),
            mqtt.isConnected() ? "OK" : "disconnected",
            rs485.getBytesReceived());
    }
}
