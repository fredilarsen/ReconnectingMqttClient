#pragma once
// Based on the spec http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718016

#include <PJON.h>

#ifndef ARDUINO
  #include <string>
  #define String std::string
  #include <interfaces/LINUX/TCPHelper_POSIX.h>
#endif

#ifndef SMCBUFSIZE
  #ifdef ARDUINO
    #define SMCBUFSIZE 100
  #else
    #define SMCBUFSIZE 1000
  #endif
#endif
#ifndef SMCTOPICSIZE
  #define SMCTOPICSIZE 30
#endif

// Remaining issues:
// - Buffer handling, static or dynamic, one member buffer?
// - TCPHelper or boost selectable?

typedef void(*RMCReceiveCallback)(
  const char     *topic,
  const uint8_t  *payload,
        uint16_t len,
        void     *custom_ptr
);

class ReconnectingMqttClient {
  // Fixed byte sequences
  const uint8_t   DISCONNECT_2[2] = { 14 << 4, 0 },
    PINGREQ_2[2] = { 12 << 4, 0 },
    PINGRESP_2[2] = { 13 << 4, 0 },
    CONNACK_1[1] = { 2 << 4 },
    PUBLISH_1[1] = { 3 << 4 },
    PUBACK_1[1] = { 4 << 4 },
    SUBSCRIBE_1[1] = { 0b10000010 },
    SUBACK_1[1] = { 9 << 4 },
    UNSUBSCRIBE_1[1] = { 0b10100010 },
    UNSUBACK_1[1] = { 11 << 4 },
    CONNECT_1[1] = { 1 << 4 },
    CONNECT_7[7] = { 0x00,0x04,'M','Q','T','T',0x04 }; // MQTT version 4 = 3.1.1
  static const uint16_t KEEPALIVE_S = 60, PING_TIMEOUT = 15000;
  static const uint8_t TIMEOUT_S = 10;

  String topic, client_id, user, password;
  uint8_t server_ip[4], sub_qos = 1;
  uint16_t port = 1883;

  TCPHelperClient client;
  bool enabled = true;
  uint16_t msg_id = 1;
  bool waiting_for_ping = false;
  uint32_t last_packet_in = 0, last_packet_out = 0;
  int8_t last_connect_error = 0x7F; // Unknown
  RMCReceiveCallback receive_callback = NULL;
  void *custom_ptr = NULL; // Custom data for the callback, for example a pointer to a derived class object
  char topicbuf[SMCTOPICSIZE];
  uint8_t buffer[SMCBUFSIZE];

  void init_system() {
#ifdef _WIN32
    WSAData wsaData; WSAStartup(MAKEWORD(2, 2), &wsaData); // Load Winsock
#endif
  }

  void cleanup_system() {
#ifdef _WIN32
    WSACleanup(); // Cleanup Winsock
#endif
  }

  void response_delay() {
    client.flush();
#ifdef ARDUINO
    delay(1); // Arduino needs some time between sending a request and reading the response
#endif
  }    

  // Write a header into the start of the buffer and return the number of bytes written
  uint16_t put_header(const uint8_t header, uint8_t *buf, const uint16_t len) {
    uint16_t l = len;
    uint8_t pos = 0, v;
    buf[pos++] = header;
    do { v = l % 0xF0; l /= 0xF0; buf[pos++] = l > 0 ? v | 0x80 : v; } while (l != 0);
    return pos;
  }

  // Write an UTF-8 text into the buffer at the given offset, return number of bytes written
  uint16_t put_string(const char *text, uint8_t *buf, const uint16_t pos) {
    uint16_t len = (uint16_t)strlen(text), p = pos;
    if (len == 0) return 0;
    buf[p++] = len >> 8;
    buf[p++] = len & 0xFF;
    memcpy(&buf[p], text, len);
    return 2 + len;
  }

  bool send_disconnect() { return write_to_socket(DISCONNECT_2, 2); }
  bool send_pingreq() { waiting_for_ping = true; return write_to_socket(PINGREQ_2, 2); }
  bool send_pingresp() { return write_to_socket(PINGRESP_2, 2); }

  uint32_t inactivity_time() {
    uint32_t in = last_packet_in == 0 ? 1000000000 : (uint32_t)(millis() - last_packet_in),
             out = last_packet_out == 0 ? 1000000000 : (uint32_t)(millis() - last_packet_out);
    return in < out ? in : out;
  }

  bool write_to_socket(const uint8_t *buf, const uint16_t len) {
    if (client.connected()) {
      uint16_t remain = len;
      while (remain > 0) {
        int written = client.write(buf, remain);
        if (written <= 0) break;
        remain -= written;
      }
      bool ok = remain == 0;
#ifdef MQTT_DEBUGPRINT
      printf("%u Sent packet %u len %d\n", millis(), buf[0], len);
#endif
      if (ok) last_packet_out = millis();
      return ok;
    }
    return false;
  }

  bool read_from_socket(uint8_t *buf, const uint16_t len, const uint16_t startpos = 0, bool blocking = true) {
    if (client.connected()) {
      uint16_t remain = len, pos = startpos;
      uint32_t start = millis();
      while (remain > 0) {
        int n = client.read(&buf[pos], remain);
        if (n == -1) break;
        if (n == 0 && (uint32_t)(millis() - start) > (uint32_t)TIMEOUT_S * 1000ul) break;
        if (!blocking && n == 0 && remain == len) break; // available() sometimes gives false positive, exit if nothing
        if (n == 0) delay(1);
        remain -= n;
      }
      return remain == 0;
    }
    return false;
  }

  bool read_packet_from_socket(uint8_t *buf, const uint16_t bufsize, uint16_t &packet_len, 
                               uint16_t &payload_len, bool blocking = true) {
    packet_len = payload_len = buf[0] = 0;
    if (!read_from_socket(buf, 2, 0, blocking)) return false;
    payload_len = buf[1] & 0x7F; // Remove upper bit
    uint16_t pos = 2;
    // Read length of payload
    uint32_t scaling = 1;
    while (buf[pos - 1] & 0x80) { // If uppermost bit is set
      if (!read_from_socket(buf, 1, pos++)) return false;
      scaling *= 0x80;
      payload_len += (buf[pos - 1] & 0x7F)*scaling;
    }
    // Read payload
    if (payload_len > 0) {
      if (pos + payload_len >= bufsize) return false; // Too big
      if (!read_from_socket(buf, payload_len, pos)) return false;
      pos += payload_len;
    }
    packet_len = pos;
    last_packet_in = millis();
#ifdef MQTT_DEBUGPRINT
    printf("%u Received packet %u len %d\n", millis(), buf[0], packet_len);
#endif
    return packet_len > 1;
  }

  bool socket_connect() {
    if (!client.connect(server_ip, port)) { delay(100); return false; }
    
    // Compose packet
    uint16_t len = 0, payloadsize = (uint16_t) (10 + (client_id.length() + 2)
      + (user.length() > 0 ? user.length() + 2 : 0)
      + (password.length() > 0 ? password.length() + 2 : 0));
    len += put_header(CONNECT_1[0], buffer, payloadsize);
    memcpy(&buffer[len], CONNECT_7, 7);
    len += 7;
    uint8_t flags = 0x02; // Clean session, No will
    if (user.length() > 0) flags |= password.length() > 0 ? 0x80 | (0x80 >> 1) : 0x80;
    buffer[len++] = flags;
    buffer[len++] = KEEPALIVE_S >> 8;
    buffer[len++] = KEEPALIVE_S & 0xFF;
    len += put_string(client_id.c_str(), buffer, len);
    len += put_string(user.c_str(), buffer, len);
    len += put_string(password.c_str(), buffer, len);
    last_connect_error = 0x7F;
    last_packet_in = millis();
    if (write_to_socket(buffer, len)) {
      response_delay();
      uint16_t packet_len, payload_len;
      if (read_packet_from_socket(buffer, sizeof buffer, packet_len, payload_len)) {
        if (packet_len == 4 && buffer[0] == CONNACK_1[0]) {
          if (buffer[3] == 0) {
            // Subscribe if a topic has been set
            if (topic.length() > 0) {
              bool ok = send_subscribe(topic.c_str(), sub_qos);
              if (!ok) stop();
            }
            return client.connected();
          }
          else last_connect_error = buffer[3]; // Got an error code
        }
      }
    }
    return false;
  }

  void handle_publish(const uint8_t *buf, const uint16_t packet_len, const uint16_t payload_len) {    
    if (receive_callback) {
      uint16_t pos = packet_len - payload_len;
      uint8_t s0 = buf[pos++], s1 = buf[pos++];
      uint16_t textlen = (s0 << 8) | s1;
      memcpy(topicbuf, &buf[pos], textlen);
      topicbuf[textlen] = 0; // Null terminator
      pos += textlen;

      if (buf[0] & 0b00000110) { // QOS1 or QOS2
        // Extract message id and send a PUBACK
        uint16_t msg_id = (buf[pos] << 8) | buf[pos+1];
        uint8_t sendbuf[4];
        sendbuf[0] = PUBACK_1[0];
        sendbuf[1] = 2;
        sendbuf[2] = buf[pos++];
        sendbuf[3] = buf[pos++];
        write_to_socket(sendbuf, 4);
      }
      receive_callback(topicbuf, &buf[pos], packet_len - pos, custom_ptr);
    }
  }

  void send_ping_if_needed() {
    if (inactivity_time() > PING_TIMEOUT) {
      if (waiting_for_ping) { stop(); start(); }
      else send_pingreq();
    }
  }

  bool send_subscribe(const char *topic, const uint8_t qos, bool unsubscribe = false) {
    if (client.connected()) {
      uint16_t payload_len = 2 + ((uint16_t)strlen(topic) + 2) + (unsubscribe ? 0 : 1);
      uint16_t len = put_header(unsubscribe ? UNSUBSCRIBE_1[0] : SUBSCRIBE_1[0], buffer, payload_len);
      if (++msg_id == 0) msg_id++; // Avoid 0
      buffer[len++] = msg_id >> 8;
      buffer[len++] = msg_id & 0xFF;
      len += put_string(topic, buffer, len);
      if (!unsubscribe) buffer[len++] = qos;
      if (write_to_socket(buffer, len)) {
        // Read SUBACK or UNSUBACK
        response_delay();
        uint16_t packet_len, payload_len;
        if (read_packet_from_socket(buffer, sizeof buffer, packet_len, payload_len, false)) {
          if (packet_len == (unsubscribe ? 4 : 5) && buffer[0] == (unsubscribe ? UNSUBACK_1[0] : SUBACK_1[0])) {
            uint16_t mess_id = (buffer[2] << 8) | buffer[3];
            if (!unsubscribe && buffer[4] > 2) return false; // Return code
            return mess_id == msg_id;
          }
        }
      }
    }
    return false;
  }

public:
  ReconnectingMqttClient() {}
  ReconnectingMqttClient(const uint8_t server_ip[4], const uint16_t server_port, const char *client_id) {
    set_address(server_ip, server_port, client_id);
  }
  ~ReconnectingMqttClient() { stop(); }

  void set_address(const uint8_t server_ip[4], const uint16_t server_port, const char *client_id) {
    memcpy(this->server_ip, server_ip, 4); port = server_port; this->client_id = client_id; start();
  }
  void set_receive_callback(RMCReceiveCallback callback, void *custom_pointer) { 
    receive_callback = callback; custom_ptr = custom_pointer; 
  }

  bool publish(const char *topic, const uint8_t *payload, const uint16_t payloadlen, const  bool retain, const uint8_t qos) {
    if (connect()) {
      uint16_t total = ((uint16_t) strlen(topic) + 2) + (qos > 0 ? 2 : 0) + payloadlen, 
               len = put_header(PUBLISH_1[0], buffer, total);
      if (retain) buffer[0] |= 1;
      buffer[0] |= qos << 1; // Add QOS into second or third bit
      len += put_string(topic, buffer, len);
      if (qos > 0) {
        if (++msg_id == 0) msg_id++;
        buffer[len++] = msg_id >> 8;
        buffer[len++] = msg_id & 0xFF;
      }
      memcpy(&buffer[len], payload, payloadlen);
      len += payloadlen;
      bool ok = write_to_socket(buffer, len);

      // Read PUBACK if QOS>0
      if (ok && qos > 0) {
        response_delay();
        ok = false;
        uint16_t packet_len, payload_len;
        if (read_packet_from_socket(buffer, sizeof buffer, packet_len, payload_len)) {
          if (packet_len == 4 && buffer[0] == PUBACK_1[0]) {
            uint16_t mess_id = (buffer[2] << 8) | buffer[3];
            ok = (mess_id == msg_id);
          }
        }
      }
      return ok;
    }
    return false;
  }

  bool subscribe(const char *topic, const uint8_t qos = 1) {
    if (this->topic.c_str()[0]) unsubscribe();
    this->topic = topic;
    sub_qos = qos;
    return send_subscribe(this->topic.c_str(), qos, false);
  }
  bool unsubscribe() { 
    bool ok = send_subscribe(this->topic.c_str(), 0, true); 
    this->topic = "";
    return ok;
  }

  void update() {
    if (connect()) {
      send_ping_if_needed();
      int16_t avail = client.available();
      if (avail > 0) {
        uint16_t packet_len, payload_len;
        if (read_packet_from_socket(buffer, sizeof buffer, packet_len, payload_len, false) && packet_len > 1) {
          if ((buffer[0] & PUBLISH_1[0]) == PUBLISH_1[0]) handle_publish(buffer, packet_len, payload_len);
          else if (buffer[0] == PINGREQ_2[0]) send_pingresp();
          else if (buffer[0] == PINGRESP_2[0]) waiting_for_ping = false;
#ifdef MQTT_DEBUGPRINT
          else printf("%u Received UNKNOWN packet %u len %d\n", millis(), buffer[0], packet_len);
#endif
        }
      }
    }
  }

  void stop() {
    if (this->topic.length() > 0) send_subscribe(this->topic.c_str(), true); 
    send_disconnect(); 
    client.stop(); 
    enabled = false;
    cleanup_system();
  }
  void start() { enabled = true; last_packet_in = last_packet_out = millis(); init_system(); }
  bool connect() { 
    if (!client.connected() && enabled) return socket_connect();
    return client.connected();
  }
  bool is_connected() { if (!client.connected()) client.stop(); return client.connected(); }
  
  char *topic_buf() { return topicbuf; } // Allow temporary access for composing outgoing topic, saving memory
}; 