"""Kết nối MQTT (paho 2.x, CallbackAPIVersion.VERSION2) giữa server và broker Mosquitto."""
import logging

import paho.mqtt.client as mqtt

log = logging.getLogger("mqtt")

class MqttLink:
    def __init__(self, host, port=1883, prefix="wp"):
        # prefix: "wp" với broker riêng; chuỗi ngẫu nhiên khi dùng broker công cộng (phải khớp firmware)
        self.host, self.port, self.prefix = host, port, prefix
        self.t_cmd, self.t_status, self.t_event = (f"{prefix}/esp/{t}" for t in ("cmd", "status", "event"))
        self.on_status = lambda payload: None
        self.on_event = lambda payload: None
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"{prefix}-server", clean_session=True)
        c.on_connect = self._on_connect
        c.on_disconnect = self._on_disconnect
        c.on_message = self._on_message
        c.reconnect_delay_set(1, 5)
        self.client = c

    def start(self):
        # connect_async: broker chưa chạy thì server vẫn khởi động, loop tự nối lại
        self.client.connect_async(self.host, self.port, keepalive=10)
        self.client.loop_start()

    def stop(self):
        self.client.disconnect()
        self.client.loop_stop()

    def publish(self, cmd):
        # QoS0, không retain: mất kết nối -> lệnh bị bỏ, không bao giờ gửi muộn
        if not self.client.is_connected():
            return False
        ok = self.client.publish(self.t_cmd, cmd, qos=0, retain=False).rc == mqtt.MQTT_ERR_SUCCESS
        log.info("cmd %s -> %s", cmd, "sent" if ok else "FAILED")
        return ok

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        log.info("broker %s:%s prefix=%s connect: %s", self.host, self.port, self.prefix, reason_code)
        if reason_code == 0:
            client.subscribe([(self.t_status, 0), (self.t_event, 0)])

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        log.warning("mất kết nối broker: %s", reason_code)
        self.on_status("OFFLINE")  # không còn biết ESP ra sao -> coi như offline

    def _on_message(self, client, userdata, msg):
        payload = msg.payload.decode("utf-8", "replace")
        log.info("%s %s", msg.topic, payload)
        if msg.topic == self.t_status:
            self.on_status(payload)
        elif msg.topic == self.t_event:
            self.on_event(payload)
