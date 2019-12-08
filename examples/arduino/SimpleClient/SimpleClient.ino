#define PJON_INCLUDE_ETCP
#include <ReconnectingMqttClient.h>

void receive_callback(const char *topic, const uint8_t *payload, uint16_t len, void *) {
  Serial.print("Received topic: "); Serial.print(topic); 
  Serial.print(", message: "); Serial.write(payload, len); 
  Serial.println();
}

uint8_t ip[] = { 127,0,0,1 };
ReconnectingMqttClient client(ip, 1883, "reconnectingclient");

void setup() {
  // Test publishing of a retained message
  client.set_receive_callback(receive_callback, NULL);
  bool ok = client.publish("hellotopic", (uint8_t*) "hihello", 7, true, 1);
  Serial.println(ok ? "Publish succeeded.\n" : "publish failed.\n");

  // Subscribe
  client.subscribe("hellotopic", 1);
}

void loop() {
  client.update();
}

