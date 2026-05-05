/*
 * mqtt_handler.h
 *
 * MQTT client wrapper for the Pentair pool controller.
 *
 * Responsibilities:
 *   - Maintain a persistent connection to the HA MQTT broker with automatic
 *     reconnection (backs off at MQTT_RECONNECT_INTERVAL to avoid hammering).
 *   - Publish PoolState fields as retained MQTT topics at most once every
 *     PUBLISH_INTERVAL ms so burst packets don't flood the broker.
 *   - On each (re)connection, publish Home Assistant MQTT auto-discovery
 *     configs so all entities appear in HA automatically with no manual YAML.
 *   - Subscribe to command topics and invoke the registered callback so
 *     main.cpp can act on pump/chlorinator/restart commands.
 *
 * PubSubClient requires a static callback function, so MqttHandler uses a
 * singleton (_instance) to route incoming messages to the live object.
 */

#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include "pentair_protocol.h"

// Signature for the command callback registered via onCommand().
// topic: full MQTT topic string, e.g. "pentair/pump/speed/set"
// payload: null-terminated command string, e.g. "Normal"
using MqttCommandCallback = std::function<void(const char* topic, const char* payload)>;

class MqttHandler {
public:
    explicit MqttHandler(WiFiClient& wifiClient);

    // Configure broker connection parameters and register the static callback.
    // Does NOT connect — connection happens lazily on the first loop() call.
    void begin(const char* server, uint16_t port,
               const char* user, const char* password,
               const char* clientId);

    // Must be called every Arduino loop(): handles reconnection and processes
    // incoming MQTT messages.
    void loop();

    // Publish all PoolState fields as retained MQTT topics.
    // Rate-limited to PUBLISH_INTERVAL; always fires once after reconnect.
    void publishState(const PoolState& state);

    // Publish Home Assistant MQTT auto-discovery config payloads.
    // Called automatically on every (re)connect so HA picks up new entities
    // after a firmware update without requiring a manual HA restart.
    void publishDiscovery();

    // Register the function that main.cpp uses to handle incoming commands.
    void onCommand(MqttCommandCallback callback) { _cmdCallback = callback; }

    bool isConnected() { return _mqtt.connected(); }

    // Force the next publishState() call to bypass the rate-limit timer.
    // Call this after sending a command so HA reflects the change immediately.
    void forceNextPublish() { _firstPublish = true; }

private:
    // Attempt one broker connection; on success calls subscribe() and publishDiscovery().
    void reconnect();

    // Subscribe to all writable command topics (pump speed, chlorinator output, restart).
    void subscribe();

    // Static trampoline required by PubSubClient — bounces through _instance to mqttCallback.
    static void mqttCallback(char* topic, byte* payload, unsigned int length);

    PubSubClient _mqtt;

    // Broker credentials stored from begin() — pointers into config.h string literals
    const char* _user;
    const char* _password;
    const char* _clientId;

    // Timestamp of the last broker connection attempt (used for back-off)
    unsigned long _lastReconnectAttempt;

    // Registered handler for incoming command payloads
    MqttCommandCallback _cmdCallback;

    // Minimum interval between consecutive publishState() calls, in milliseconds.
    // Prevents the broker from being flooded when many packets arrive at once.
    unsigned long _lastPublish;
    static const unsigned long PUBLISH_INTERVAL = 2000;

    // Retained for future change-detection logic; currently unused in diff checks
    PoolState _prevState;

    // True after connect/reconnect; forces an immediate publish regardless of PUBLISH_INTERVAL
    bool _firstPublish;

    // Singleton pointer — set in constructor; used by the static mqttCallback trampoline
    static MqttHandler* _instance;
};
