/*
 * mqtt_handler.cpp
 *
 * MQTT client wrapper: publishes PoolState to Home Assistant and handles
 * incoming commands from HA (pump speed, chlorinator output, ESP32 restart).
 *
 * On each (re)connection the handler publishes Home Assistant MQTT
 * auto-discovery config payloads so entities appear automatically in HA.
 * Stale discovery configs from older firmware versions are cleared by
 * publishing empty retained payloads to their topics.
 *
 * State is published at most once every PUBLISH_INTERVAL ms (2 s) to avoid
 * flooding the broker when packets arrive in bursts. All publishes use the
 * retain flag so HA always shows the last known value after a restart.
 */

#include "mqtt_handler.h"
#include "config.h"
#include <ArduinoJson.h>

// ============================================================
// Helpers
// ============================================================

// Returns true if eventMillis is non-zero and was within the last 60 seconds
static bool recentActivity(unsigned long eventMillis) {
    return eventMillis > 0 && (millis() - eventMillis) < 60000;
}

// ============================================================
// Static member initialization
// ============================================================

// Singleton pointer used by the static PubSubClient callback to reach the instance
MqttHandler* MqttHandler::_instance = nullptr;

// ============================================================
// Construction / initialization
// ============================================================

MqttHandler::MqttHandler(WiFiClient& wifiClient)
    : _mqtt(wifiClient)
    , _user(nullptr)
    , _password(nullptr)
    , _clientId(nullptr)
    , _lastReconnectAttempt(0)
    , _lastPublish(0)
    , _firstPublish(true)
{
    _instance  = this;
    _prevState = {};
}

void MqttHandler::begin(const char* server, uint16_t port,
                         const char* user, const char* password,
                         const char* clientId) {
    _user     = user;
    _password = password;
    _clientId = clientId;
    _mqtt.setServer(server, port);
    _mqtt.setCallback(mqttCallback);
    // 1024 bytes is large enough for the biggest discovery payload (~512 bytes JSON + overhead)
    _mqtt.setBufferSize(1024);
}

// ============================================================
// Connection management
// ============================================================

void MqttHandler::loop() {
    if (!_mqtt.connected()) {
        unsigned long now = millis();
        // Back off reconnect attempts to avoid hammering the broker
        if (now - _lastReconnectAttempt > MQTT_RECONNECT_INTERVAL) {
            _lastReconnectAttempt = now;
            reconnect();
        }
        return;
    }
    _mqtt.loop();
}

void MqttHandler::reconnect() {
    Serial.println("[MQTT] Attempting connection...");
    if (_mqtt.connect(_clientId, _user, _password)) {
        Serial.println("[MQTT] Connected!");
        subscribe();
        publishDiscovery();
        _firstPublish = true;  // Force an immediate state publish after reconnect
    } else {
        Serial.printf("[MQTT] Failed, rc=%d\n", _mqtt.state());
    }
}

void MqttHandler::subscribe() {
    // IntelliConnect ignores RS485 commands, so we only subscribe to topics
    // for devices we can actually control: pump (0x60) and chlorinator (0x50).
    _mqtt.subscribe(MQTT_TOPIC_PREFIX "/pump/speed/set");
    _mqtt.subscribe(MQTT_TOPIC_PREFIX "/chlorinator/set");
    _mqtt.subscribe(MQTT_TOPIC_PREFIX "/restart");
    Serial.println("[MQTT] Subscribed to command topics");
}

// PubSubClient requires a static callback; bounce through the singleton to reach instance state
void MqttHandler::mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (!_instance || !_instance->_cmdCallback) return;
    char msg[64];
    size_t copyLen = (length < sizeof(msg) - 1) ? length : sizeof(msg) - 1;
    memcpy(msg, payload, copyLen);
    msg[copyLen] = '\0';
    _instance->_cmdCallback(topic, msg);
}

// ============================================================
// State publishing
// ============================================================

void MqttHandler::publishState(const PoolState& state) {
    if (!_mqtt.connected()) return;

    // Rate-limit publishes; always allow the first one after a reconnect
    unsigned long now = millis();
    if (now - _lastPublish < PUBLISH_INTERVAL && !_firstPublish) return;
    _lastPublish  = now;
    _firstPublish = false;

    char buf[16];

    // Pool on/off is inferred from pump RPM (IntelliConnect does not broadcast circuit state)
    _mqtt.publish(MQTT_TOPIC_PREFIX "/pool/state", state.poolOn ? "ON" : "OFF", true);

    // Water temp comes from IntelliChlor CHLOR_EXTENDED_STATUS; skip until first valid reading
    // so HA retains the last known value rather than showing 0 on boot
    if (state.waterTemp > 0) {
        snprintf(buf, sizeof(buf), "%d", state.waterTemp);
        _mqtt.publish(MQTT_TOPIC_PREFIX "/water_temp", buf, true);
    }

    // Pump
    _mqtt.publish(MQTT_TOPIC_PREFIX "/pump/state", state.pumpRunning ? "ON" : "OFF", true);

    snprintf(buf, sizeof(buf), "%d", state.pumpRPM);
    _mqtt.publish(MQTT_TOPIC_PREFIX "/pump/rpm", buf, true);

    snprintf(buf, sizeof(buf), "%d", state.pumpWatts);
    _mqtt.publish(MQTT_TOPIC_PREFIX "/pump/watts", buf, true);

    // Index matches PumpSpeed enum: 0=Stop, 1=Low, 2=Normal, 3=High, 4=Max
    const char* speedNames[] = {"Stop", "Low", "Normal", "High", "Max"};
    _mqtt.publish(MQTT_TOPIC_PREFIX "/pump/speed/state", speedNames[state.pumpSpeedPreset], true);

    // Chlorinator
    snprintf(buf, sizeof(buf), "%d", state.chlorOutput);
    _mqtt.publish(MQTT_TOPIC_PREFIX "/chlorinator/output", buf, true);

    snprintf(buf, sizeof(buf), "%d", state.chlorSaltPPM);
    _mqtt.publish(MQTT_TOPIC_PREFIX "/chlorinator/salt_ppm", buf, true);

    _mqtt.publish(MQTT_TOPIC_PREFIX "/chlorinator/state",      state.chlorActive      ? "ON" : "OFF", true);
    _mqtt.publish(MQTT_TOPIC_PREFIX "/chlorinator/low_salt",   state.chlorLowSalt     ? "ON" : "OFF", true);
    _mqtt.publish(MQTT_TOPIC_PREFIX "/chlorinator/check_cell", state.chlorInspectCell ? "ON" : "OFF", true);

    // Connectivity health: ON = heard from within the last 60 seconds, OFF = silent
    _mqtt.publish(MQTT_TOPIC_PREFIX "/rs485_alive",   recentActivity(state.lastRs485Activity) ? "ON" : "OFF", true);
    _mqtt.publish(MQTT_TOPIC_PREFIX "/pentair_alive", recentActivity(state.lastPumpUpdate)    ? "ON" : "OFF", true);
}

// ============================================================
// Home Assistant MQTT auto-discovery
// ============================================================

void MqttHandler::publishDiscovery() {
    JsonDocument doc;

    // Shared device block — links all entities to one HA device card
    auto deviceInfo = [&]() {
        doc["device"]["identifiers"][0] = "pentair_controller";
        doc["device"]["name"]           = "Pentair Pool Controller";
        doc["device"]["manufacturer"]   = "Pentair";
        doc["device"]["model"]          = "IntelliConnect";
    };

    auto publishSensor = [&](const char* id, const char* name,
                              const char* stateTopic, const char* unit,
                              const char* deviceClass) {
        doc.clear();
        doc["name"]        = name;
        doc["unique_id"]   = String("pentair_") + id;
        doc["state_topic"] = stateTopic;
        if (unit)        doc["unit_of_measurement"] = unit;
        if (deviceClass) doc["device_class"]        = deviceClass;
        deviceInfo();

        char configTopic[128], payload[512];
        snprintf(configTopic, sizeof(configTopic), "homeassistant/sensor/pentair/%s/config", id);
        serializeJson(doc, payload, sizeof(payload));
        _mqtt.publish(configTopic, payload, true);
    };

    auto publishSelect = [&](const char* id, const char* name,
                              const char* stateTopic, const char* cmdTopic,
                              std::initializer_list<const char*> options) {
        doc.clear();
        doc["name"]          = name;
        doc["unique_id"]     = String("pentair_") + id;
        doc["state_topic"]   = stateTopic;
        doc["command_topic"] = cmdTopic;
        int i = 0;
        for (auto opt : options) doc["options"][i++] = opt;
        deviceInfo();

        char configTopic[128], payload[512];
        snprintf(configTopic, sizeof(configTopic), "homeassistant/select/pentair/%s/config", id);
        serializeJson(doc, payload, sizeof(payload));
        _mqtt.publish(configTopic, payload, true);
    };

    auto publishButton = [&](const char* id, const char* name, const char* cmdTopic) {
        doc.clear();
        doc["name"]          = name;
        doc["unique_id"]     = String("pentair_") + id;
        doc["command_topic"] = cmdTopic;
        doc["payload_press"] = "PRESS";
        deviceInfo();

        char configTopic[128], payload[512];
        snprintf(configTopic, sizeof(configTopic), "homeassistant/button/pentair/%s/config", id);
        serializeJson(doc, payload, sizeof(payload));
        _mqtt.publish(configTopic, payload, true);
    };

    auto publishBinarySensor = [&](const char* id, const char* name,
                                    const char* stateTopic, const char* deviceClass) {
        doc.clear();
        doc["name"]         = name;
        doc["unique_id"]    = String("pentair_") + id;
        doc["state_topic"]  = stateTopic;
        doc["expire_after"] = 30;  // Mark unavailable if no update within 30s (catches dead ESP32)
        if (deviceClass) doc["device_class"] = deviceClass;
        deviceInfo();

        char configTopic[128], payload[512];
        snprintf(configTopic, sizeof(configTopic), "homeassistant/binary_sensor/pentair/%s/config", id);
        serializeJson(doc, payload, sizeof(payload));
        _mqtt.publish(configTopic, payload, true);
    };

    // Purge stale retained discovery configs left by previous firmware versions.
    // Publishing an empty payload to a retained topic removes it from the broker.
    const char* staleTopics[] = {
        "homeassistant/switch/pentair/pool/config",
        "homeassistant/switch/pentair/pool_light/config",
        "homeassistant/switch/pentair/cleaner/config",
        "homeassistant/switch/pentair/heater/config",
        "homeassistant/number/pentair/heater_setpoint/config",
        "homeassistant/sensor/pentair/air_temp/config",
        "homeassistant/sensor/pentair/heater_active/config",
        "homeassistant/sensor/pentair/last_rs485/config",
        "homeassistant/sensor/pentair/last_packet/config",
    };
    for (const char* t : staleTopics) _mqtt.publish(t, "", true);

    // Pool on/off is read-only (inferred from pump RPM; IntelliConnect ignores SET commands)
    publishSensor("pool",       "Pool",       MQTT_TOPIC_PREFIX "/pool/state", nullptr, nullptr);
    publishSensor("water_temp", "Water Temp", MQTT_TOPIC_PREFIX "/water_temp", "°F", "temperature");

    // Pump — speed control is writable; state/RPM/watts are read-only sensors
    publishSensor("pump_state", "Pump",       MQTT_TOPIC_PREFIX "/pump/state", nullptr, nullptr);
    publishSensor("pump_rpm",   "Pump RPM",   MQTT_TOPIC_PREFIX "/pump/rpm",   "RPM",  nullptr);
    publishSensor("pump_watts", "Pump Power", MQTT_TOPIC_PREFIX "/pump/watts", "W",    "power");
    publishSelect("pump_speed", "Pump Speed",
                  MQTT_TOPIC_PREFIX "/pump/speed/state", MQTT_TOPIC_PREFIX "/pump/speed/set",
                  {"Stop", "Low", "Normal", "High", "Max"});

    // Chlorinator — output % is writable; all other fields are read-only sensors
    publishSensor("chlor_output",   "Chlorinator Output", MQTT_TOPIC_PREFIX "/chlorinator/output",     "%",   nullptr);
    publishSensor("salt_ppm",       "Salt Level",         MQTT_TOPIC_PREFIX "/chlorinator/salt_ppm",   "ppm", nullptr);
    publishSensor("chlor_state",    "Chlorinator Active", MQTT_TOPIC_PREFIX "/chlorinator/state",      nullptr, nullptr);
    publishSensor("chlor_low_salt", "Salt Low",           MQTT_TOPIC_PREFIX "/chlorinator/low_salt",   nullptr, nullptr);
    publishSensor("chlor_check",    "Check Cell",         MQTT_TOPIC_PREFIX "/chlorinator/check_cell", nullptr, nullptr);

    // Connectivity health — ON=active within 60s, OFF=silent; device_class=connectivity
    // shows as "Connected / Disconnected" in HA
    publishBinarySensor("rs485_alive",   "RS485 Bus",      MQTT_TOPIC_PREFIX "/rs485_alive",   "connectivity");
    publishBinarySensor("pentair_alive", "Pentair Pump",   MQTT_TOPIC_PREFIX "/pentair_alive", "connectivity");

    // Remote restart button
    publishButton("restart", "Restart ESP32", MQTT_TOPIC_PREFIX "/restart");

    Serial.println("[MQTT] Published HA discovery configs");
}
