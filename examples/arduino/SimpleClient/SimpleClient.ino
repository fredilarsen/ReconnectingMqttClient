#define PJON_INCLUDE_ETCP
#define MQTT_DEBUGPRINT
#include <Arduino.h>
#include <ReconnectingMqttClient.h>

uint32_t receive_cnt = 0, start = 0;

void receive(const char *topic, const uint8_t *payload, const uint16_t len, void *custom_ptr) {
  // Make a zero-terminated copy of the payload, assuming it is a string
  char buf[100];
  uint8_t l = len > sizeof buf ? sizeof buf : len;
  memcpy(buf, payload, l);
  buf[l] = 0;
  receive_cnt++; // Count the number of packets received
  Serial.print("Received topic: "); Serial.print(topic); 
  Serial.print(", message: "); Serial.println(buf);
}

bool timeout() {
  if (start == 0) start = millis();
  else return ((uint32_t)(millis() - start) > 60000);
  return false;
}

uint8_t ip[] = { 127,0,0,1 };
ReconnectingMqttClient client(ip, 1883, "reconnectingclient");

void setup() {
  // Test publishing of a retained message
  client.set_receive_callback(receive, NULL);
  bool ok = client.publish("hellotopic", (uint8_t*) "hihello", 7, true, 1);
  Serial.println(ok ? "Publish succeeded.\n" : "publish failed.\n");

  // Subscribe
  client.subscribe("hellotopic", 1);
}

void loop() {
  client.update();
}

