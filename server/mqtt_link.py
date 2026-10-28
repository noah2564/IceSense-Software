"""Thin paho-mqtt wrapper. All it does: subscribe, hand messages to a callback, publish."""
import logging
import uuid

import paho.mqtt.client as mqtt

log = logging.getLogger("mqtt")


class MqttLink:
    def __init__(self, host: str, port: int, on_message, user: str = "", password: str = "",
                 tls: bool = False, subscribe: str = "ice/v1/+/+"):
        self.host, self.port, self._subscribe = host, port, subscribe
        self._on_message = on_message
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  client_id=f"ice-server-{uuid.uuid4().hex[:8]}")
        if user:
            self.client.username_pw_set(user, password)
        if tls:
            self.client.tls_set()          # system CA store; use tls_set(ca_certs=...) for a private CA
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)
        self.client.on_connect = self._connected
        self.client.on_disconnect = lambda *a, **k: log.warning("MQTT disconnected")
        self.client.on_message = self._message

    def _connected(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("MQTT connect failed: %s", reason_code)
            return
        log.info("MQTT connected to %s:%s", self.host, self.port)
        client.subscribe(self._subscribe, qos=1)   # re-subscribe after every (re)connect

    def _message(self, client, userdata, msg):
        try:
            self._on_message(msg.topic, msg.payload)
        except Exception:                           # never let one bad message kill the network thread
            log.exception("error handling message on %s", msg.topic)

    def start(self):
        self.client.connect_async(self.host, self.port, keepalive=30)
        self.client.loop_start()

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()

    def publish(self, topic: str, payload: str, qos: int = 1) -> bool:
        return self.client.publish(topic, payload, qos=qos).rc == mqtt.MQTT_ERR_SUCCESS
