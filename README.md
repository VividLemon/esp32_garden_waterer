# ESP32 Garden Waterer

An ESP32 that controls the water valves for four garden beds and reports soil moisture for each one to Home Assistant.

| Bed | Valve relay | Moisture sensor |
| --- | --- | --- |
| 1 | `GPIO23` | `GPIO36` |
| 2 | `GPIO22` | `GPIO34` |
| 3 | `GPIO21` | `GPIO35` |
| 4 | `GPIO19` | `GPIO32` |

Valve relays are active-LOW. Moisture is read as raw ADC counts (0–4095) every 10 seconds.

## Versions

I started with the C++ Arduino sketch, then wrote ESPHome and MicroPython versions to see what they'd be like. All three do the same job:

| Version | Files | Talks to Home Assistant via |
| --- | --- | --- |
| C++ (Arduino) | `esp32_garden_waterer.ino`, `secrets.example.h` | MQTT + discovery |
| ESPHome | `garden-controller.yaml`, `garden-bed.yaml` | Native API (MQTT optional) |
| MicroPython | `main.py`, `secrets.example.py` | MQTT + discovery |

The ESPHome and MicroPython versions also add a failsafe: a valve left on for more than 30 minutes turns itself off, even if Home Assistant or Wi-Fi is down.

> **If this is ever used again, use the ESPHome version.** It's the least code, adds a bed with a single `packages:` entry, gets OTA updates and a fallback hotspot for free, and doesn't need reconnect or discovery logic maintained by hand.

## Example setup (ESPHome)

Copy `secrets.yaml.example` to `secrets.yaml` next to `garden-controller.yaml` and fill it in:

```yaml
wifi_ssid: "YourNetwork"
wifi_password: "YourWifiPassword"
fallback_ap_password: "change-me-123"

# Generate with: openssl rand -base64 32   (or let the ESPHome dashboard create one)
api_encryption_key: "REPLACE_WITH_32_BYTE_BASE64_KEY"
ota_password: "change-me"

# Only needed if you enable the mqtt: block
mqtt_broker: "192.168.1.10"
mqtt_username: ""
mqtt_password: ""
```

Then flash it over USB (later updates can go over Wi-Fi):

```bash
esphome run garden-controller.yaml
```

Home Assistant should discover the device automatically; add it with the same `api_encryption_key`.

### Other versions

- **C++:** copy `secrets.example.h` to `secrets.h`, fill it in, and build with the Arduino IDE or `arduino-cli` (ESP32 core + `PubSubClient`).
- **MicroPython:** copy `secrets.example.py` to `secrets.py`, fill it in, and upload both files with `mpremote cp main.py secrets.py :`.
