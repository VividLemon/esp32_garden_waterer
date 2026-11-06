# ESP32 Garden Waterer

Controls the water valves for four garden beds and reports each bed's soil moisture to Home Assistant. A valve left on for more than 30 minutes turns itself off.

## Use the YAML

If you come across this, use **`garden-controller.yaml`**.

1. In Home Assistant, open **ESPHome Device Builder** and create a new device (board: **Generic ESP32 Board**).
2. Replace its config with the contents of `garden-controller.yaml`.
3. In the Builder's **Secrets** editor, add:

   ```yaml
   wifi_ssid: "YourNetwork"
   wifi_password: "YourWifiPassword"
   garden_controller__encryption_key: "REPLACE_WITH_32_BYTE_BASE64_KEY" # openssl rand -base64 32
   ```

4. **Install**. The first flash is over USB; updates after that go over Wi-Fi.

Wiring (valve relays are active-LOW):

| Bed | Valve | Moisture sensor |
| --- | --- | --- |
| 1 | `GPIO23` | `GPIO36` |
| 2 | `GPIO22` | `GPIO34` |
| 3 | `GPIO21` | `GPIO35` |
| 4 | `GPIO19` | `GPIO32` |

## Other files

`esp32_garden_waterer.ino` (C++) and `main.py` (MicroPython) are earlier experiments that do the same thing over MQTT. I started with C++, then tried ESPHome and Python to compare.
