# MicroPython port of the ESP32 garden controller.
#
# Same behaviour and MQTT topics as the Arduino sketch, so Home Assistant
# doesn't notice the swap:
#   garden/bed/<n>/valve/set               ON / OFF commands
#   garden/bed/<n>/valve/state             retained ON / OFF
#   garden/bed/<n>/sensor/<m>/moisture     retained raw ADC (0-4095), every 10 s
#   garden/status                          retained online / offline (LWT)
#   homeassistant/switch|sensor/...        retained discovery payloads
#
# Needs umqtt.simple (bundled with the standard ESP32 firmware; otherwise
# `mpremote mip install umqtt.simple`).

import time
import json
import network
import machine
import ubinascii
from machine import Pin, ADC
from umqtt.simple import MQTTClient

import secrets  # see secrets.example.py

# (valve pin, [moisture sensor pins]) per bed, same as Bed beds[] in the sketch
BEDS = [
    (23, [36]),
    (22, [34]),
    (21, [35]),
    (19, [32]),
]

WIFI_RETRY_MS = 10_000
MQTT_RETRY_MS = 5_000
MQTT_PING_MS = 30_000          # umqtt.simple doesn't send keepalives itself
SENSOR_PUBLISH_MS = 10_000
COMMAND_DEBOUNCE_MS = 150
SENSOR_SAMPLES = 10
SENSOR_SAMPLE_DELAY_MS = 5

# Failsafe (not in the original sketch): a valve left on longer than this
# shuts itself off even if Home Assistant / Wi-Fi is down. 0 disables it.
MAX_VALVE_RUN_MS = 30 * 60 * 1000

# Reboot if the main loop stalls. Set False while developing at the REPL;
# once started, the watchdog can't be stopped.
USE_WATCHDOG = True
WATCHDOG_MS = 30_000

BASE_TOPIC = "garden/bed/"
DISCOVERY_PREFIX = "homeassistant"
STATUS_TOPIC = b"garden/status"
DEVICE_NAME = "ESP32 Garden Controller"
DEVICE_ID = "esp32_garden_01"

DEVICE_INFO = {
    "identifiers": [DEVICE_ID],
    "name": DEVICE_NAME,
    "manufacturer": "DIY",
    "model": "ESP32 Garden",
}


def valve_set_topic(n):
    return "{}{}/valve/set".format(BASE_TOPIC, n).encode()


def valve_state_topic(n):
    return "{}{}/valve/state".format(BASE_TOPIC, n).encode()


def sensor_topic(n, m):
    return "{}{}/sensor/{}/moisture".format(BASE_TOPIC, n, m).encode()


class GardenController:
    def __init__(self):
        now = time.ticks_ms()

        self.valves = []          # Pin per bed
        self.sensors = []         # [ADC, ...] per bed
        self.valve_on = []        # bool per bed
        self.valve_on_since = []  # ticks per bed
        self.last_cmd = []        # ticks per bed
        self.cmd_topics = {}      # set-topic bytes -> bed index

        for i, (valve_pin, sensor_pins) in enumerate(BEDS):
            # Active-LOW relay: start HIGH (closed valve) with no boot glitch
            self.valves.append(Pin(valve_pin, Pin.OUT, value=1))
            adcs = []
            for p in sensor_pins:
                adc = ADC(Pin(p))
                adc.atten(ADC.ATTN_11DB)   # full ~0-3.3 V range
                adc.width(ADC.WIDTH_12BIT)
                adcs.append(adc)
            self.sensors.append(adcs)
            self.valve_on.append(False)
            self.valve_on_since.append(now)
            self.last_cmd.append(time.ticks_add(now, -COMMAND_DEBOUNCE_MS))
            self.cmd_topics[valve_set_topic(i + 1)] = i

        self.wlan = network.WLAN(network.STA_IF)
        self.wlan.active(True)
        self.wifi_was_connected = False
        self._wifi_connect()
        self.last_wifi_attempt = now

        client_id = b"esp32-garden-" + ubinascii.hexlify(machine.unique_id())
        self.mqtt = MQTTClient(
            client_id,
            secrets.MQTT_SERVER,
            port=getattr(secrets, "MQTT_PORT", 1883),
            user=getattr(secrets, "MQTT_USER", None),
            password=getattr(secrets, "MQTT_PASSWORD", None),
            keepalive=60,
        )
        self.mqtt.set_callback(self.on_message)
        self.mqtt.set_last_will(STATUS_TOPIC, b"offline", retain=True, qos=1)
        self.mqtt_connected = False
        self.last_mqtt_attempt = time.ticks_add(now, -MQTT_RETRY_MS)
        self.last_ping = now
        self.last_sensor_publish = now

        print("Controller booted")

    # ---------------- Wi-Fi / MQTT ----------------

    def _wifi_connect(self):
        try:
            self.wlan.connect(secrets.WIFI_SSID, secrets.WIFI_PASSWORD)
        except OSError as e:
            print("WiFi connect error:", e)

    def _drop_mqtt(self):
        # Close the socket without a clean DISCONNECT so the broker publishes
        # our "offline" last will.
        self.mqtt_connected = False
        try:
            self.mqtt.sock.close()
        except Exception:
            pass

    def _publish(self, topic, payload, retain=True):
        if not self.mqtt_connected:
            return False
        try:
            self.mqtt.publish(topic, payload, retain=retain)
            return True
        except OSError as e:
            print("Publish failed on", topic, e)
            self._drop_mqtt()
            return False

    def _connect_mqtt(self):
        print("MQTT connecting...")
        try:
            self.mqtt.connect()
        except OSError as e:
            print("MQTT connect failed:", e)
            self._drop_mqtt()
            return

        self.mqtt_connected = True
        self.last_ping = time.ticks_ms()
        print("MQTT connected")
        try:
            self.mqtt.publish(STATUS_TOPIC, b"online", retain=True)
            for topic in self.cmd_topics:
                self.mqtt.subscribe(topic)
        except OSError as e:
            print("MQTT setup failed:", e)
            self._drop_mqtt()
            return

        self.publish_discovery()
        self.publish_all_valve_states()

    def service_network(self, now):
        if not self.wlan.isconnected():
            if self.wifi_was_connected:
                self.wifi_was_connected = False
                print("WiFi disconnected")
                self._drop_mqtt()
            if time.ticks_diff(now, self.last_wifi_attempt) >= WIFI_RETRY_MS:
                self.last_wifi_attempt = now
                print("WiFi reconnect attempt...")
                self._wifi_connect()
            return

        if not self.wifi_was_connected:
            self.wifi_was_connected = True
            print("WiFi connected. IP:", self.wlan.ifconfig()[0])

        if not self.mqtt_connected:
            if time.ticks_diff(now, self.last_mqtt_attempt) >= MQTT_RETRY_MS:
                self.last_mqtt_attempt = now
                self._connect_mqtt()
            return

        try:
            self.mqtt.check_msg()  # non-blocking; calls on_message
            if time.ticks_diff(now, self.last_ping) >= MQTT_PING_MS:
                self.mqtt.ping()
                self.last_ping = now
        except OSError as e:
            print("MQTT connection lost:", e)
            self._drop_mqtt()

    # ---------------- Discovery ----------------

    def publish_discovery(self):
        for i, (_, sensor_pins) in enumerate(BEDS):
            n = i + 1
            valve_cfg = {
                "name": "Garden Bed {} Valve".format(n),
                "unique_id": "{}_bed_{}_valve".format(DEVICE_ID, n),
                "command_topic": valve_set_topic(n).decode(),
                "state_topic": valve_state_topic(n).decode(),
                "payload_on": "ON",
                "payload_off": "OFF",
                "state_on": "ON",
                "state_off": "OFF",
                "availability_topic": STATUS_TOPIC.decode(),
                "payload_available": "online",
                "payload_not_available": "offline",
                "device": DEVICE_INFO,
            }
            topic = "{}/switch/{}_bed_{}_valve/config".format(DISCOVERY_PREFIX, DEVICE_ID, n)
            self._publish(topic.encode(), json.dumps(valve_cfg).encode())

            for s in range(len(sensor_pins)):
                m = s + 1
                sensor_cfg = {
                    "name": "Garden Bed {} Sensor {} Moisture".format(n, m),
                    "unique_id": "{}_bed_{}_sensor_{}_moisture".format(DEVICE_ID, n, m),
                    "state_topic": sensor_topic(n, m).decode(),
                    "unit_of_measurement": "ADC",
                    "availability_topic": STATUS_TOPIC.decode(),
                    "payload_available": "online",
                    "payload_not_available": "offline",
                    "device": DEVICE_INFO,
                }
                topic = "{}/sensor/{}_bed_{}_sensor_{}_moisture/config".format(
                    DISCOVERY_PREFIX, DEVICE_ID, n, m)
                self._publish(topic.encode(), json.dumps(sensor_cfg).encode())

    # ---------------- Valves ----------------

    def set_valve(self, i, on):
        self.valves[i].value(0 if on else 1)   # active-LOW
        self.valve_on[i] = on
        self.valve_on_since[i] = time.ticks_ms()
        self._publish(valve_state_topic(i + 1), b"ON" if on else b"OFF")
        print("Valve bed", i + 1, "->", "ON" if on else "OFF")

    def publish_all_valve_states(self):
        for i, on in enumerate(self.valve_on):
            self._publish(valve_state_topic(i + 1), b"ON" if on else b"OFF")

    def on_message(self, topic, msg):
        if msg == b"ON":
            on = True
        elif msg == b"OFF":
            on = False
        else:
            print("Ignored invalid command payload on topic:", topic)
            return

        i = self.cmd_topics.get(topic)
        if i is None:
            print("Ignored command on unexpected topic:", topic)
            return

        now = time.ticks_ms()
        if time.ticks_diff(now, self.last_cmd[i]) < COMMAND_DEBOUNCE_MS:
            print("Ignored rapid repeat command for bed", i + 1)
            return

        self.last_cmd[i] = now
        self.set_valve(i, on)

    def service_valves(self, now):
        if not MAX_VALVE_RUN_MS:
            return
        for i, on in enumerate(self.valve_on):
            if on and time.ticks_diff(now, self.valve_on_since[i]) >= MAX_VALVE_RUN_MS:
                print("Bed", i + 1, "valve hit max run time, turning off")
                self.set_valve(i, False)

    # ---------------- Sensors ----------------

    @staticmethod
    def read_sensor(adc):
        total = 0
        for _ in range(SENSOR_SAMPLES):
            total += adc.read()
            time.sleep_ms(SENSOR_SAMPLE_DELAY_MS)
        return total // SENSOR_SAMPLES

    def service_sensors(self, now):
        if time.ticks_diff(now, self.last_sensor_publish) < SENSOR_PUBLISH_MS:
            return
        self.last_sensor_publish = now
        if not self.mqtt_connected:
            return
        for i, adcs in enumerate(self.sensors):
            for s, adc in enumerate(adcs):
                value = self.read_sensor(adc)
                self._publish(sensor_topic(i + 1, s + 1), str(value).encode())

    # ---------------- Main loop ----------------

    def run(self):
        wdt = machine.WDT(timeout=WATCHDOG_MS) if USE_WATCHDOG else None
        while True:
            if wdt:
                wdt.feed()
            now = time.ticks_ms()
            self.service_network(now)
            self.service_valves(now)
            self.service_sensors(now)
            time.sleep_ms(5)


GardenController().run()
