#pragma once

// Copy this file to secrets.h and replace values for local development.
// CI can use this file directly when secrets.h is intentionally absent.

constexpr const char* WIFI_SSID = "CHANGE_ME_SSID";
constexpr const char* WIFI_PASSWORD = "CHANGE_ME_PASSWORD";
constexpr const char* MQTT_SERVER_IP = "127.0.0.1";
constexpr uint16_t MQTT_SERVER_PORT = 1883;
