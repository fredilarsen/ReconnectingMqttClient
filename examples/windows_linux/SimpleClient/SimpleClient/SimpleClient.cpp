#define PJON_INCLUDE_DUDP
#define MQTT_DEBUGPRINT
#include <ReconnectingMqttClient.h>

uint32_t receive_cnt = 0, start = 0;

void receive(const char *topic, const uint8_t *payload, const uint16_t len, void *custom_ptr) {
  // Make a zero-terminated copy of the payload, assuming it is a string
  char buf[100];
  uint8_t l = len > sizeof buf ? sizeof buf : len;
  memcpy(buf, payload, l);
  buf[l] = 0;
  receive_cnt++; // Count the number of packets received
  printf("Received: topic: %s, message: %s\n", topic, buf);
}

bool timeout() {
  if (start == 0) start = millis();
  else return ((uint32_t)(millis() - start) > 60000);
  return false;
}

void main() {
  // Test publishing of a retained message
  uint8_t ip[] = { 127,0,0,1 };
  ReconnectingMqttClient client1(ip, 1883, "reconnectingclient");
  client1.set_receive_callback(receive, NULL);
  bool ok = client1.publish("hellotopic", (uint8_t*) "hihello", 7, true, 1);
  printf(ok ? "Publish succeeded.\n" : "publish failed.\n");

  // Subscribe and wait a while for published messages, then unsubscribe
  client1.subscribe("hellotopic", 1);
  while (client1.is_connected() && !timeout()) client1.update();
  client1.unsubscribe("hellotopic");

  // Subscribe and wait for messages again, the stop the client
  start = 0;
  client1.subscribe("hellotopic");
  while (client1.is_connected() && !timeout()) client1.update();
  client1.stop();
}

