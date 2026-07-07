// mesh.cpp — ESP-NOW mesh implementation.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>
#include "device.h"    // SERIAL_DEBUG
#include "mesh.h"

// Tiny framing so a stray ESP-NOW packet from something else can't
// impersonate a command.
static const uint8_t MAGIC = 0xA5;
struct __attribute__((packed)) Packet { uint8_t magic; uint8_t cmd; };
static uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Ring buffer for received commands; the receive callback runs on the
// Wi-Fi task, drained by loop() via meshPoll().
static volatile uint8_t rxBuf[8];
static volatile uint8_t rxHead = 0, rxTail = 0;

// R3 dedup state — shared across IR and mesh paths.
static uint8_t  lastCmd   = 0xFF;
static uint32_t lastActMs = 0;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t*, const uint8_t* data, int len) {
#else
static void onRecv(const uint8_t*, const uint8_t* data, int len) {
#endif
  if (len != (int)sizeof(Packet)) return;
  const Packet* p = (const Packet*)data;
  if (p->magic != MAGIC) return;
  uint8_t n = (rxHead + 1) & 7;
  if (n != rxTail) { rxBuf[rxHead] = p->cmd; rxHead = n; }
}

void meshBroadcast(uint8_t cmd) {
  Packet p{MAGIC, cmd};
  esp_now_send(BROADCAST, (uint8_t*)&p, sizeof(p));
}

bool meshPoll(uint8_t* cmd) {
  if (rxTail == rxHead) return false;
  *cmd = rxBuf[rxTail];
  rxTail = (rxTail + 1) & 7;
  return true;
}

bool dedupAccept(uint8_t cmd) {
  uint32_t now = millis();
  if (cmd == lastCmd && (now - lastActMs) < DEDUP_MS) return false;
  lastCmd = cmd; lastActMs = now;
  return true;
}

void meshBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (esp_now_init() != ESP_OK) {
#if SERIAL_DEBUG
    Serial.println(F("ERROR: esp_now_init"));
#endif
    return;
  }
  esp_now_register_recv_cb(onRecv);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}
