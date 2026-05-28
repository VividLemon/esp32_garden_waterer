#include <WiFi.h>
#include <PubSubClient.h>
#include <string.h>
#include "secrets.h"

struct Bed {
  int valvePin;
  const int* sensors;
  int sensorCount;
};

constexpr int bed1Sensors[] = {36};
constexpr int bed2Sensors[] = {34};
constexpr int bed3Sensors[] = {35};
constexpr int bed4Sensors[] = {32};

Bed beds[] = {
  {23, bed1Sensors, 1},
  {22, bed2Sensors, 1},
  {21, bed3Sensors, 1},
  {19, bed4Sensors, 1}
};

constexpr size_t NUM_BEDS = sizeof(beds) / sizeof(beds[0]);
constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long MQTT_RETRY_MS = 5000;
constexpr unsigned long SENSOR_PUBLISH_MS = 10000;
constexpr unsigned long COMMAND_DEBOUNCE_MS = 150;
constexpr size_t TOPIC_BUF_LEN = 96;
constexpr size_t PAYLOAD_BUF_LEN = 1024;

WiFiClient espClient;
PubSubClient mqtt(espClient);

const char* BASE_TOPIC = "garden/bed/";
const char* DISCOVERY_PREFIX = "homeassistant";
const char* STATUS_TOPIC = "garden/status";

const char* DEVICE_NAME = "ESP32 Garden Controller";
const char* DEVICE_ID = "esp32_garden_01";

bool valveStates[NUM_BEDS] = {false};
unsigned long lastValveCommandMs[NUM_BEDS] = {0};
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastMQTTAttemptMs = 0;
bool wifiWasConnected = false;
char mqttClientId[32] = {0};
char valveSetTopics[NUM_BEDS][TOPIC_BUF_LEN] = {{0}};
char valveStateTopics[NUM_BEDS][TOPIC_BUF_LEN] = {{0}};

void setValve(size_t bedIndex, bool on);
bool ensureWiFiConnected();
void ensureMQTTConnected();
void buildClientId();
void buildTopics();
void buildSensorTopic(size_t bedIndex, size_t sensorIndex, char* out, size_t outLen);
bool parseValveCommand(const byte* payload, unsigned int length, bool& on);
bool bedHasSensors(size_t bedIndex);

void buildClientId() {
  uint32_t chipId = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFF);
  snprintf(mqttClientId, sizeof(mqttClientId), "esp32-garden-%06lX", static_cast<unsigned long>(chipId));
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  unsigned long now = millis();
  if (now - lastWiFiAttemptMs >= WIFI_RETRY_MS) {
    lastWiFiAttemptMs = now;
    Serial.println("WiFi reconnect attempt...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  return false;
}

int readSensorPin(int pin) {
  int total = 0;
  for (int i = 0; i < 10; i++) {
    total += analogRead(pin);
    delay(5);
  }
  return total / 10;
}

void buildTopics() {
  for (size_t i = 0; i < NUM_BEDS; i++) {
    unsigned int bedNumber = static_cast<unsigned int>(i + 1);
    snprintf(valveSetTopics[i], sizeof(valveSetTopics[i]), "%s%u/valve/set", BASE_TOPIC, bedNumber);
    snprintf(valveStateTopics[i], sizeof(valveStateTopics[i]), "%s%u/valve/state", BASE_TOPIC, bedNumber);
  }
}

void buildSensorTopic(size_t bedIndex, size_t sensorIndex, char* out, size_t outLen) {
  unsigned int bedNumber = static_cast<unsigned int>(bedIndex + 1);
  unsigned int sensorNumber = static_cast<unsigned int>(sensorIndex + 1);
  snprintf(out, outLen, "%s%u/sensor/%u/moisture", BASE_TOPIC, bedNumber, sensorNumber);
}

bool bedHasSensors(size_t bedIndex) {
  if (bedIndex >= NUM_BEDS) {
    return false;
  }

  const Bed& bed = beds[bedIndex];
  return bed.sensorCount > 0 && bed.sensors != nullptr;
}

void setValve(size_t bedIndex, bool on) {
  if (bedIndex >= NUM_BEDS) {
    return;
  }

  digitalWrite(beds[bedIndex].valvePin, on ? LOW : HIGH);
  valveStates[bedIndex] = on;

  bool ok = mqtt.publish(valveStateTopics[bedIndex], on ? "ON" : "OFF", true);
  if (!ok) {
    Serial.print("Valve state publish failed for bed ");
    Serial.println(static_cast<unsigned int>(bedIndex + 1));
  }

  Serial.print("Valve bed ");
  Serial.print(static_cast<unsigned int>(bedIndex + 1));
  Serial.print(" -> ");
  Serial.println(on ? "ON" : "OFF");
}

bool parseValveCommand(const byte* payload, unsigned int length, bool& on) {
  if (length == 2 && payload[0] == 'O' && payload[1] == 'N') {
    on = true;
    return true;
  }

  if (length == 3 && payload[0] == 'O' && payload[1] == 'F' && payload[2] == 'F') {
    on = false;
    return true;
  }

  return false;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  bool on = false;
  bool payloadValid = parseValveCommand(payload, length, on);

  if (!payloadValid) {
    Serial.print("Ignored invalid command payload on topic: ");
    Serial.println(topic);
    return;
  }

  for (size_t i = 0; i < NUM_BEDS; i++) {
    if (strcmp(topic, valveSetTopics[i]) == 0) {
      unsigned long now = millis();
      if (now - lastValveCommandMs[i] < COMMAND_DEBOUNCE_MS) {
        Serial.print("Ignored rapid repeat command for bed ");
        Serial.println(static_cast<unsigned int>(i + 1));
        return;
      }

      lastValveCommandMs[i] = now;
      setValve(i, on);
      return;
    }
  }

  Serial.print("Ignored command on unexpected topic: ");
  Serial.println(topic);
}

void publishValveDiscovery(int bed) {
  size_t bedIndex = static_cast<size_t>(bed);
  unsigned int bedNumber = static_cast<unsigned int>(bedIndex + 1);
  char topic[TOPIC_BUF_LEN] = {0};
  char payload[PAYLOAD_BUF_LEN] = {0};

  snprintf(topic, sizeof(topic), "%s/switch/%s_bed_%u_valve/config", DISCOVERY_PREFIX, DEVICE_ID, bedNumber);
  snprintf(payload, sizeof(payload),
           "{\"name\":\"Garden Bed %u Valve\","
           "\"unique_id\":\"%s_bed_%u_valve\","
           "\"command_topic\":\"%s\","
           "\"state_topic\":\"%s\","
           "\"payload_on\":\"ON\","
           "\"payload_off\":\"OFF\","
           "\"state_on\":\"ON\","
           "\"state_off\":\"OFF\","
           "\"availability_topic\":\"%s\","
           "\"payload_available\":\"online\","
           "\"payload_not_available\":\"offline\","
           "\"device\":{"
             "\"identifiers\":[\"%s\"],"
             "\"name\":\"%s\","
             "\"manufacturer\":\"DIY\","
             "\"model\":\"ESP32 Garden\""
           "}}",
           bedNumber,
           DEVICE_ID,
           bedNumber,
           valveSetTopics[bedIndex],
           valveStateTopics[bedIndex],
           STATUS_TOPIC,
           DEVICE_ID,
           DEVICE_NAME);

  bool ok = mqtt.publish(topic, payload, true);
  if (!ok) {
    Serial.print("Valve discovery publish failed for bed ");
    Serial.println(bedNumber);
  }
}

void publishSensorDiscovery(int bed) {
  size_t bedIndex = static_cast<size_t>(bed);
  if (!bedHasSensors(bedIndex)) {
    return;
  }

  unsigned int bedNumber = static_cast<unsigned int>(bedIndex + 1);
  const Bed& bedDef = beds[bedIndex];

  for (int sensor = 0; sensor < bedDef.sensorCount; sensor++) {
    unsigned int sensorNumber = static_cast<unsigned int>(sensor + 1);
    char stateTopic[TOPIC_BUF_LEN] = {0};
    char topic[TOPIC_BUF_LEN] = {0};
    char payload[PAYLOAD_BUF_LEN] = {0};

    buildSensorTopic(bedIndex, static_cast<size_t>(sensor), stateTopic, sizeof(stateTopic));
    snprintf(topic, sizeof(topic), "%s/sensor/%s_bed_%u_sensor_%u_moisture/config", DISCOVERY_PREFIX, DEVICE_ID, bedNumber, sensorNumber);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Garden Bed %u Sensor %u Moisture\","
             "\"unique_id\":\"%s_bed_%u_sensor_%u_moisture\","
             "\"state_topic\":\"%s\","
             "\"unit_of_measurement\":\"ADC\","
             "\"availability_topic\":\"%s\","
             "\"payload_available\":\"online\","
             "\"payload_not_available\":\"offline\","
             "\"device\":{"
               "\"identifiers\":[\"%s\"],"
               "\"name\":\"%s\","
               "\"manufacturer\":\"DIY\","
               "\"model\":\"ESP32 Garden\""
             "}}",
             bedNumber,
             sensorNumber,
             DEVICE_ID,
             bedNumber,
             sensorNumber,
             stateTopic,
             STATUS_TOPIC,
             DEVICE_ID,
             DEVICE_NAME);

    bool ok = mqtt.publish(topic, payload, true);
    if (!ok) {
      Serial.print("Sensor discovery publish failed for bed ");
      Serial.print(bedNumber);
      Serial.print(" sensor ");
      Serial.println(sensorNumber);
    }
  }
}

void publishDiscovery() {
  for (size_t i = 0; i < NUM_BEDS; i++) {
    publishValveDiscovery(static_cast<int>(i));
    publishSensorDiscovery(static_cast<int>(i));
  }
}

void publishAllValveStates() {
  for (size_t i = 0; i < NUM_BEDS; i++) {
    bool ok = mqtt.publish(valveStateTopics[i], valveStates[i] ? "ON" : "OFF", true);
    if (!ok) {
      Serial.print("Failed to publish valve state for bed ");
      Serial.println(static_cast<unsigned int>(i + 1));
    }
  }
}

void publishBeds() {
  if (!mqtt.connected()) {
    return;
  }

  for (size_t i = 0; i < NUM_BEDS; i++) {
    if (!bedHasSensors(i)) {
      continue;
    }

    const Bed& bed = beds[i];
    for (int sensor = 0; sensor < bed.sensorCount; sensor++) {
      int moisture = readSensorPin(bed.sensors[sensor]);

      char topic[TOPIC_BUF_LEN] = {0};
      char payload[16] = {0};
      buildSensorTopic(i, static_cast<size_t>(sensor), topic, sizeof(topic));
      snprintf(payload, sizeof(payload), "%d", moisture);

      bool ok = mqtt.publish(topic, payload, true);
      if (!ok) {
        Serial.print("Moisture publish failed for bed ");
        Serial.print(static_cast<unsigned int>(i + 1));
        Serial.print(" sensor ");
        Serial.println(sensor + 1);
      }
    }
  }
}

void publishAll() {
  publishDiscovery();
  publishAllValveStates();
}

void ensureMQTTConnected() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (now - lastMQTTAttemptMs < MQTT_RETRY_MS) {
    return;
  }

  lastMQTTAttemptMs = now;
  Serial.println("MQTT connecting...");

  if (mqtt.connect(mqttClientId, STATUS_TOPIC, 1, true, "offline")) {
    Serial.println("MQTT connected");

    bool statusOk = mqtt.publish(STATUS_TOPIC, "online", true);
    if (!statusOk) {
      Serial.print("Status publish failed, mqtt rc=");
      Serial.println(mqtt.state());
    }

    for (size_t i = 0; i < NUM_BEDS; i++) {
      bool subOk = mqtt.subscribe(valveSetTopics[i]);
      if (!subOk) {
        Serial.print("Subscribe failed for bed ");
        Serial.print(static_cast<unsigned int>(i + 1));
        Serial.print(", mqtt rc=");
        Serial.println(mqtt.state());
      }
    }

    publishAll();
  } else {
    Serial.print("MQTT connect failed, rc=");
    Serial.println(mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  buildClientId();

  analogSetAttenuation(ADC_11db);

  buildTopics();

  for (size_t i = 0; i < NUM_BEDS; i++) {
    pinMode(beds[i].valvePin, OUTPUT);
    digitalWrite(beds[i].valvePin, HIGH);
    valveStates[i] = false;
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
  Serial.println("Controller booted");

  mqtt.setBufferSize(2048);
  mqtt.setServer(MQTT_SERVER_IP, MQTT_SERVER_PORT);
  mqtt.setCallback(mqttCallback);
}

void loop() {
  bool wifiConnected = ensureWiFiConnected();

  if (wifiConnected && !wifiWasConnected) {
    wifiWasConnected = true;
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else if (!wifiConnected && wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("WiFi disconnected");
    if (mqtt.connected()) {
      mqtt.publish(STATUS_TOPIC, "offline", true);
      mqtt.disconnect();
    }
  }

  if (wifiConnected) {
    ensureMQTTConnected();
    if (mqtt.connected()) {
      mqtt.loop();
    }
  }

  static unsigned long lastPublish = 0;

  if (millis() - lastPublish > SENSOR_PUBLISH_MS) {
    lastPublish = millis();
    publishBeds();
  }
}
