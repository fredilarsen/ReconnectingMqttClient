#define PJON_INCLUDE_DUDP
#define MQTT_DEBUGPRINT
#include <ReconnectingMqttClient.h>

bool received = false;
long start = 0;

void receive(const char *topic, const uint8_t *payload, const uint16_t len, void *custom_ptr) {
  char buf[100];
  uint8_t l = len > sizeof buf ? sizeof buf : len;
  memcpy(buf, payload, l);
  buf[l] = 0;
  received = true;
  printf("Received: topic: %s, message: %s\n", topic, buf);
}

void threadsafe_receive(const char *topic, const uint8_t *payload, const uint16_t len, void *custom_ptr) {
  char buf[100];
  uint8_t l = len > sizeof buf ? sizeof buf : len;
  memcpy(buf, payload, l);
  buf[l] = 0;
  received = true;
  printf("Received: topic: %s, message: %s\n", topic, buf);
}

bool timeout() {
  if (start == 0) start = millis();
  else return ((uint32_t)(millis() - start) > 60000);
  return false;
}

void test_foreground() {
  // Test the simple mqtt client publish and 
  uint8_t ip[] = { 127,0,0,1 };
  ReconnectingMqttClient client1(ip, 1883, "reconnectingclient");
  client1.set_receive_callback(receive, NULL);
  bool ok = client1.publish("hellotopic", (uint8_t*) "hihello", 7, true, 1);
  printf(ok ? "Publish succeeded.\n" : "publish failed.\n");

  client1.subscribe("hellotopic", 1);
  while (client1.is_connected() && !timeout()) client1.update();
  client1.unsubscribe("hellotopic");

  start = 0;
  client1.subscribe("hellotopic");
  while (client1.is_connected() && !timeout()) client1.update();
  client1.stop();
}

void test_background() {
/*
  BackgroundMqttClient client("127.0.0.1", 1883, "mqtttransfer");
  client.set_receive_callback(receive, NULL);
  client.set_threadsafe_receive_callback(threadsafe_receive, NULL);
  mqtt.subscribe("hellotopic, 2);
  client.start();
  client.publish("hellotopic", "MqttTransfer!", 13);
  while (!timeout()) { client.update(); } // Only needed if using non-threadsafe callback
  client.stop();
*/
}

int main() {
  // Initialize Winsock
  WSAData wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);

  test_foreground();

  test_background();

  // Cleanup Winsock
  WSACleanup();
}

