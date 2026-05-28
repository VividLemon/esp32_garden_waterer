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

constexpr int NUM_BEDS = 4;
constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long MQTT_RETRY_MS = 5000;
constexpr unsigned long SENSOR_PUBLISH_MS = 10000;
constexpr unsigned long COMMAND_DEBOUNCE_MS = 150;

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

void setValve(int bedIndex, bool on);
bool ensureWiFiConnected();
void ensureMQTTConnected();
void buildClientId();
bool parseValveCommand(const byte* payload, unsigned int length, bool& on);
bool bedHasSensors(int bedIndex);

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

int readBedMoisture(int bedIndex) {
  if (bedIndex < 0 || bedIndex >= NUM_BEDS) {
    return -1;
  }

  Bed& bed = beds[bedIndex];
  if (bed.sensorCount <= 0 || bed.sensors == nullptr) {
    return -1;
  }

  int total = 0;

  for (int i = 0; i < bed.sensorCount; i++) {
    total += readSensorPin(bed.sensors[i]);
  }

  return total / bed.sensorCount;
}

bool bedHasSensors(int bedIndex) {
  if (bedIndex < 0 || bedIndex >= NUM_BEDS) {
    return false;
  }

  const Bed& bed = beds[bedIndex];
  return bed.sensorCount > 0 && bed.sensors != nullptr;
}

void setValve(int bedIndex, bool on) {
  if (bedIndex < 0 || bedIndex >= NUM_BEDS) {
    return;
  }

  digitalWrite(beds[bedIndex].valvePin, on ? LOW : HIGH);
  valveStates[bedIndex] = on;

  String topic = String(BASE_TOPIC) + String(bedIndex + 1) + "/valve/state";
  bool ok = mqtt.publish(topic.c_str(), on ? "ON" : "OFF", true);
  if (!ok) {
    Serial.print("Valve state publish failed for bed ");
    Serial.println(bedIndex + 1);
  }

  Serial.print("Valve bed ");
  Serial.print(bedIndex + 1);
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

  for (int i = 0; i < NUM_BEDS; i++) {
    String expected = String(BASE_TOPIC) + String(i + 1) + "/valve/set";

    if (strcmp(topic, expected.c_str()) == 0) {
      unsigned long now = millis();
      if (now - lastValveCommandMs[i] < COMMAND_DEBOUNCE_MS) {
        Serial.print("Ignored rapid repeat command for bed ");
        Serial.println(i + 1);
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
  String topic = String(DISCOVERY_PREFIX) + "/switch/garden_bed_" + String(bed + 1) + "/config";

  String payload =
  "{"
    "\"name\":\"Garden Bed " + String(bed + 1) + " Valve\","
    "\"unique_id\":\"garden_bed_" + String(bed + 1) + "_valve\","
    "\"command_topic\":\"garden/bed/" + String(bed + 1) + "/valve/set\","
    "\"state_topic\":\"garden/bed/" + String(bed + 1) + "/valve/state\","
    "\"payload_on\":\"ON\","
    "\"payload_off\":\"OFF\","
    "\"state_on\":\"ON\","
    "\"state_off\":\"OFF\","
    "\"availability_topic\":\"" + String(STATUS_TOPIC) + "\","
    "\"payload_available\":\"online\","
    "\"payload_not_available\":\"offline\","
    "\"device\":{"
      "\"identifiers\":[\"" + String(DEVICE_ID) + "\"],"
      "\"name\":\"" + String(DEVICE_NAME) + "\","
      "\"manufacturer\":\"DIY\","
      "\"model\":\"ESP32 Garden\""
    "}"
  "}";

  bool ok = mqtt.publish(topic.c_str(), payload.c_str(), true);
  if (!ok) {
    Serial.print("Valve discovery publish failed for bed ");
    Serial.println(bed + 1);
  }
}

void publishSensorDiscovery(int bed) {
  if (!bedHasSensors(bed)) {
    return;
  }

  String topic = String(DISCOVERY_PREFIX) + "/sensor/garden_bed_" + String(bed + 1) + "_moisture/config";

  String payload =
  "{"
    "\"name\":\"Garden Bed " + String(bed + 1) + " Moisture\","
    "\"unique_id\":\"garden_bed_" + String(bed + 1) + "_moisture\","
    "\"state_topic\":\"garden/bed/" + String(bed + 1) + "/moisture\","
    "\"unit_of_measurement\":\"ADC\","
    "\"availability_topic\":\"" + String(STATUS_TOPIC) + "\","
    "\"payload_available\":\"online\","
    "\"payload_not_available\":\"offline\","
    "\"device\":{"
      "\"identifiers\":[\"" + String(DEVICE_ID) + "\"],"
      "\"name\":\"" + String(DEVICE_NAME) + "\","
      "\"manufacturer\":\"DIY\","
      "\"model\":\"ESP32 Garden\""
    "}"
  "}";

  bool ok = mqtt.publish(topic.c_str(), payload.c_str(), true);
  if (!ok) {
    Serial.print("Sensor discovery publish failed for bed ");
    Serial.println(bed + 1);
  }
}

void publishDiscovery() {
  for (int i = 0; i < NUM_BEDS; i++) {
    publishValveDiscovery(i);
    publishSensorDiscovery(i);
  }
}

void publishAllValveStates() {
  for (int i = 0; i < NUM_BEDS; i++) {
    String topic = String(BASE_TOPIC) + String(i + 1) + "/valve/state";
    bool ok = mqtt.publish(topic.c_str(), valveStates[i] ? "ON" : "OFF", true);
    if (!ok) {
      Serial.print("Failed to publish valve state for bed ");
      Serial.println(i + 1);
    }
  }
}

void publishBeds() {
  if (!mqtt.connected()) {
    return;
  }

  for (int i = 0; i < NUM_BEDS; i++) {
    if (!bedHasSensors(i)) {
      continue;
    }

    int moisture = readBedMoisture(i);
    if (moisture < 0) {
      Serial.print("Skipping moisture publish for bed ");
      Serial.print(i + 1);
      Serial.println(" (no sensors configured)");
      continue;
    }

    String topic = String(BASE_TOPIC) + String(i + 1) + "/moisture";
    bool ok = mqtt.publish(topic.c_str(), String(moisture).c_str(), true);
    if (!ok) {
      Serial.print("Moisture publish failed for bed ");
      Serial.println(i + 1);
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

    for (int i = 0; i < NUM_BEDS; i++) {
      String topic = String(BASE_TOPIC) + String(i + 1) + "/valve/set";
      bool subOk = mqtt.subscribe(topic.c_str());
      if (!subOk) {
        Serial.print("Subscribe failed for bed ");
        Serial.print(i + 1);
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

  for (int i = 0; i < NUM_BEDS; i++) {
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
