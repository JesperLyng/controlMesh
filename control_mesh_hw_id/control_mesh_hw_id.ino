/*
 * Fan controller (ESP32) — IR + ESP-NOW mesh
 * -------------------------------------------------
 * All five nodes are IDENTICAL — no code changes per board.
 * FAN_ID (1-5) is set with jumpers to GND on three input pins:
 *
 *   GPIO_A  GPIO_B  GPIO_C  ->  ID
 *   float   float   float   ->  1   (no jumper)
 *   GND     float   float   ->  2
 *   float   GND     float   ->  3
 *   GND     GND     float   ->  4
 *   float   float   GND     ->  5
 *
 * Uses internal pullups (INPUT_PULLUP) — no external resistors needed.
 * Or solder 10 kOhm from each pin to 3.3V and use INPUT instead.
 *
 * For Arctic P14 Pro PST and other 4-pin PWM fans (25 kHz signal).
 *
 *   Keys 1-5 : select fan
 *   + / -    : one speed step up/down on the SELECTED fan
 *   POWER    : toggle the SELECTED fan on/off
 *   "0"      : turn ALL fans off
 *
 * Requires: ESP32 Arduino core (2.x or 3.x) + "IRremote" library (v4.x).
 */

#define DECODE_NEC
#include <IRremote.hpp>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------- ID pins (internal pullups, jumper to GND = bit set) ----------
const uint8_t ID_PIN_A = 16;
const uint8_t ID_PIN_B = 17;
const uint8_t ID_PIN_C = 18;

// ---------- Wi-Fi channel (all nodes MUST use the same) ----------
const uint8_t WIFI_CHANNEL = 1;

// ---------- Fan pins ----------
const uint8_t IR_PIN      = 4;
const uint8_t PWM_PIN     = 5;   // 10 kOhm pullup to 3.3V on this pin
const uint8_t SIG_LED_PIN = 19;  // blinks on a valid signal (330 Ohm in series)
const int8_t  SEL_LED_PIN = -1;  // lit when this node is selected, or -1 to disable

// ---------- Signal-LED blink (non-blocking) ----------
const uint16_t BLINK_MS = 50;
uint32_t blinkOffAt = 0;

void signalBlink() {
  digitalWrite(SIG_LED_PIN, HIGH);
  blinkOffAt = millis() + BLINK_MS;
}

void blinkUpdate() {
  if (blinkOffAt && millis() >= blinkOffAt) {
    digitalWrite(SIG_LED_PIN, LOW);
    blinkOffAt = 0;
  }
}

// ---------- IR codes (NEC command byte) ----------
const uint8_t CMD_SELECT[5] = {0x0C, 0x18, 0x5E, 0x08, 0x1C};
const uint8_t CMD_VOL_UP    = 0x15;
const uint8_t CMD_VOL_DOWN  = 0x07;
const uint8_t CMD_POWER     = 0x43;
const uint8_t CMD_ALL_OFF   = 0x16;

// ---------- Speed steps (% duty) ----------
const uint8_t LEVELS[]   = {40, 55, 70, 85, 100};
const uint8_t NUM_LEVELS = sizeof(LEVELS);

#define SERIAL_DEBUG 1

// ---------- PWM (LEDC) ----------
const uint32_t PWM_FREQ = 25000;
const uint8_t  PWM_RES  = 8;
const uint8_t  PWM_CH   = 0;

// ---------- Mesh ----------
const uint16_t DEDUP_MS = 150;
const uint8_t  MAGIC    = 0xA5;
struct __attribute__((packed)) Packet { uint8_t magic; uint8_t cmd; };
uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ---------- State ----------
uint8_t  fanID       = 1;       // read in setup()
uint8_t  selectedFan = 1;
bool     fanOn       = false;
int8_t   levelIndex  = 2;
uint8_t  lastCmd     = 0xFF;
uint32_t lastActMs   = 0;

// ---------- ESP-NOW receive queue ----------
volatile uint8_t rxBuf[8];
volatile uint8_t rxHead = 0, rxTail = 0;

// ---------------------------------------------------------
uint8_t readFanID() {
  pinMode(ID_PIN_A, INPUT_PULLUP);
  pinMode(ID_PIN_B, INPUT_PULLUP);
  pinMode(ID_PIN_C, INPUT_PULLUP);
  delay(5);                      // let pins settle

  bool a = (digitalRead(ID_PIN_A) == LOW);
  bool b = (digitalRead(ID_PIN_B) == LOW);
  bool c = (digitalRead(ID_PIN_C) == LOW);

  if (!a && !b && !c) return 1;
  if ( a && !b && !c) return 2;
  if (!a &&  b && !c) return 3;
  if ( a &&  b && !c) return 4;
  if (!a && !b &&  c) return 5;

  // Invalid jumper pattern — log a warning and fall back to ID 1
#if SERIAL_DEBUG
  Serial.println(F("WARNING: unknown ID jumper combination — falling back to ID 1"));
#endif
  return 1;
}

// ---------- PWM ----------
void pwmBegin() {
  // Drive the pin LOW as a digital output BEFORE LEDC takes over.
  // Combined with the 10 kOhm pullup to 3.3V, this minimises the window
  // in which the fan sees an undefined signal during boot.
  // The fan may briefly run at full speed (~500 ms) — accepted trade-off.
  pinMode(PWM_PIN, OUTPUT);
  digitalWrite(PWM_PIN, LOW);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PWM_PIN, PWM_FREQ, PWM_RES);
#else
  ledcSetup(PWM_CH, PWM_FREQ, PWM_RES);
  ledcAttachPin(PWM_PIN, PWM_CH);
#endif
}

void pwmWrite(uint8_t percent) {
  if (percent > 100) percent = 100;
  uint32_t duty = (uint32_t)percent * ((1u << PWM_RES) - 1) / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PWM_PIN, duty);
#else
  ledcWrite(PWM_CH, duty);
#endif
}

void applyOutput() { pwmWrite(fanOn ? LEVELS[levelIndex] : 0); }

// ---------- Dedup (R3) ----------
bool accept(uint8_t cmd) {
  uint32_t now = millis();
  if (cmd == lastCmd && (now - lastActMs) < DEDUP_MS) return false;
  lastCmd = cmd; lastActMs = now;
  return true;
}

// ---------- Command logic ----------
void applyCommand(uint8_t cmd) {
  for (uint8_t i = 0; i < 5; i++)
    if (cmd == CMD_SELECT[i]) { selectedFan = i + 1; return; }

  bool mine = (selectedFan == fanID);

  if (cmd == CMD_VOL_UP) {
    if (mine) { if (levelIndex < NUM_LEVELS - 1) levelIndex++; fanOn = true; applyOutput(); }
    return;
  }
  if (cmd == CMD_VOL_DOWN) {
    if (mine) { if (levelIndex > 0) levelIndex--; fanOn = true; applyOutput(); }
    return;
  }
  if (cmd == CMD_POWER) {
    if (mine) { fanOn = !fanOn; applyOutput(); }
    return;
  }
  if (cmd == CMD_ALL_OFF) {
    fanOn = false; applyOutput();
    return;
  }
}

// ---------- ESP-NOW ----------
void meshBroadcast(uint8_t cmd) {
  Packet p{MAGIC, cmd};
  esp_now_send(BROADCAST, (uint8_t*)&p, sizeof(p));
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t*, const uint8_t* data, int len) {
#else
void onRecv(const uint8_t*, const uint8_t* data, int len) {
#endif
  if (len != (int)sizeof(Packet)) return;
  const Packet* p = (const Packet*)data;
  if (p->magic != MAGIC) return;
  uint8_t n = (rxHead + 1) & 7;
  if (n != rxTail) { rxBuf[rxHead] = p->cmd; rxHead = n; }
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

// ---------------------------------------------------------
void setup() {
#if SERIAL_DEBUG
  Serial.begin(115200); delay(50);
#endif
  fanID = readFanID();
#if SERIAL_DEBUG
  Serial.printf("Node ready. ID=%d  channel=%d\n", fanID, WIFI_CHANNEL);
#endif
  pinMode(SIG_LED_PIN, OUTPUT);
  digitalWrite(SIG_LED_PIN, LOW);
  if (SEL_LED_PIN >= 0) { pinMode(SEL_LED_PIN, OUTPUT); digitalWrite(SEL_LED_PIN, LOW); }
  pwmBegin();
  applyOutput();
  meshBegin();
  IrReceiver.begin(IR_PIN, DISABLE_LED_FEEDBACK);
}

void loop() {
  // IR in: initial frames only — execute + rebroadcast
  if (IrReceiver.decode()) {
    auto &d = IrReceiver.decodedIRData;
    if (d.protocol != UNKNOWN && !(d.flags & IRDATA_FLAGS_IS_REPEAT)) {
      uint8_t cmd = d.command;
      if (accept(cmd)) {
        signalBlink();
        applyCommand(cmd);
        meshBroadcast(cmd);
#if SERIAL_DEBUG
        Serial.printf("IR  0x%02X  selected=%d on=%d step=%d\n",
                      cmd, selectedFan, fanOn, levelIndex);
#endif
      }
    }
    IrReceiver.resume();
  }

  // ESP-NOW in: execute, but NEVER rebroadcast
  while (rxTail != rxHead) {
    uint8_t cmd = rxBuf[rxTail];
    rxTail = (rxTail + 1) & 7;
    if (accept(cmd)) {
      signalBlink();
      applyCommand(cmd);
#if SERIAL_DEBUG
      Serial.printf("RF  0x%02X  selected=%d on=%d step=%d\n",
                    cmd, selectedFan, fanOn, levelIndex);
#endif
    }
  }

  blinkUpdate();
  if (SEL_LED_PIN >= 0) digitalWrite(SEL_LED_PIN, (selectedFan == fanID) ? HIGH : LOW);
}
