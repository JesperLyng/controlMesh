/*
 * Fan controller (ESP32) — IR + ESP-NOW mesh
 * -------------------------------------------------
 * All five nodes are IDENTICAL — no code changes per board.
 * FAN_ID (1-5) is set with jumpers to GND — see the truth table in the
 * "ID pins" section below.
 *
 * For Arctic P14 Pro PST and other 4-pin PWM fans (25 kHz signal).
 *
 *   Colors (scope selectors, tracked on every node):
 *     BLUE   : fan scope — subsequent commands apply to fans (this binary)
 *     YELLOW : LED-strip scope (not yet built — LEDs will be a separate binary)
 *     RED    : reserved for a future device class
 *     GREEN  : reserved for a future device class
 *   Pressing a color also resets selection to "all in scope".
 *
 *   Keys 1-5 : select a specific instance within the active scope
 *   "0"      : select ALL instances within the active scope
 *   + / -    : one step up/down on the current selection
 *   POWER    : toggle the current selection on/off
 *   MUTE     : force the current selection off AND reset its level to 0
 *              (a subsequent POWER-on starts from the minimum)
 *
 * Commands other than scope/select are only acted on when the active scope
 * matches THIS_NODE_SCOPE — so pressing YELLOW then MUTE is a no-op on fan
 * nodes but will kill all LEDs once LED nodes exist.
 *
 * Requires: ESP32 Arduino core (2.x or 3.x) + "IRremote" library (v4.x).
 */

#define DECODE_SAMSUNG
#include <IRremote.hpp>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------- ID pins ----------
// Every board flashes the same binary; each node identifies itself as fan
// 1..5 by reading three GPIO pins in readFanID(). The pins are configured
// as INPUT_PULLUP, so the ESP32 holds each one at 3.3V through an internal
// ~45 kOhm resistor. A pin with nothing attached reads HIGH; a pin
// jumpered to GND wins that pull-down fight and reads LOW. No external
// components are needed — just three wires and up to two jumpers per board.
//
// Three pins = 3 bits = 8 patterns; only 5 are mapped to fan IDs. The
// remaining 3 patterns fall back to ID 1 with a Serial warning, so a
// wiring mistake still produces a working (if duplicate) node. Mapping
// is arbitrary but stable — pick which physical board is which by
// fitting jumpers before power-up.
//
// Truth table (— = float / no jumper, GND = jumper to GND):
//
//   ID_PIN_A  ID_PIN_B  ID_PIN_C   -> FAN
//     —         —         —          1   (no jumper — fresh board defaults to 1)
//     GND       —         —          2
//     —         GND       —          3
//     GND       GND       —          4
//     —         —         GND        5
//
// If you prefer visible external pull-ups on a soldered PCB, tie each ID
// pin to 3.3V via a 10 kOhm resistor and change INPUT_PULLUP to INPUT in
// readFanID(). Behavior is identical.
//
// GPIO 32 / 33 / 25 are all general-purpose, all support INPUT_PULLUP,
// and are safe on both plain ESP32 and WROVER (PSRAM) dev boards — the
// PSRAM SPI on WROVER variants uses 16/17, which we deliberately avoid.
// They also sit adjacent on the 30-pin ESP32 DevKit header, so the three
// jumper wires stay in a tidy cluster.
const uint8_t ID_PIN_A = 32;
const uint8_t ID_PIN_B = 33;
const uint8_t ID_PIN_C = 25;

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

// ---------- IR codes (Samsung command byte, address 0x0E) ----------
// Codes 0x06..0x09 (keys 6-9) are captured but unmapped — reserved for
// future CMD_SELECT expansion or another device class on the same mesh.
const uint8_t CMD_SELECT[5]   = {0x01, 0x02, 0x03, 0x04, 0x05};
const uint8_t CMD_SELECT_ALL  = 0x00;  // key "0"
const uint8_t CMD_VOL_UP      = 0x14;
const uint8_t CMD_VOL_DOWN    = 0x15;
const uint8_t CMD_POWER       = 0x0C;  // toggle the current selection
const uint8_t CMD_OFF         = 0x0D;  // MUTE: force off + reset level to 0

// Scope selectors (color keys). Every node tracks activeScope so the mesh
// stays in sync, but only nodes whose THIS_NODE_SCOPE matches act on the
// non-scope/non-select commands.
const uint8_t CMD_SCOPE_RED    = 0xA0;
const uint8_t CMD_SCOPE_GREEN  = 0xA1;
const uint8_t CMD_SCOPE_LED    = 0xA2;  // YELLOW
const uint8_t CMD_SCOPE_FAN    = 0xA3;  // BLUE

enum Scope : uint8_t {
  SCOPE_FAN   = 0,
  SCOPE_LED   = 1,
  SCOPE_RED   = 2,
  SCOPE_GREEN = 3,
};

// This binary is the fan controller. LED / future-class nodes flash a
// different binary with THIS_NODE_SCOPE set accordingly.
const uint8_t THIS_NODE_SCOPE = SCOPE_FAN;

// Sentinel: selectedFan == SELECT_ALL means "apply to every node in the active scope".
const uint8_t SELECT_ALL = 0;

// ---------- Speed steps (% duty) ----------
// The user-facing scale is 0..10 (eleven discrete levels) across every scope.
// Each device class maps 0..10 to its own output range; for fans, level 0 is
// 25% duty and level 10 is 100%, linear between (7.5%-per-step, rounded).
// 25% is below the typical PC-fan stall floor — needs hardware verification
// that the actual fans start reliably from level 0 or 1.
// LED firmware (future) will map 0..10 to its own brightness range.
const uint8_t LEVELS[]   = {25, 33, 40, 48, 55, 63, 70, 78, 85, 93, 100};
const uint8_t NUM_LEVELS = sizeof(LEVELS);

#define SERIAL_DEBUG 1

// ---------- CPU frequency ----------
// 80 MHz is the lowest ESP32 setting that still keeps WiFi/ESP-NOW alive
// (40 MHz and below disable the radio). APB clock stays at 80 MHz here too,
// so LEDC PWM timing and Serial are unaffected, and IRremote's timers
// re-scale automatically. Halves idle current vs the 240 MHz default —
// matters on a boat. Do not lower further without an alternative mesh transport.
const uint32_t CPU_FREQ_MHZ = 80;

// ---------- PWM (LEDC) ----------
const uint32_t PWM_FREQ = 25000;
const uint8_t  PWM_RES  = 8;
const uint8_t  PWM_CH   = 0;

// ---------- Mesh ----------
const uint16_t DEDUP_MS      = 150;    // R3 window
const uint16_t VOL_REPEAT_MS = 333;    // R2 exception: VOL+/- ramp ~3 steps/sec while held
const uint8_t  MAGIC         = 0xA5;
struct __attribute__((packed)) Packet { uint8_t magic; uint8_t cmd; };
uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ---------- State ----------
uint8_t  fanID       = 1;                 // read in setup()
uint8_t  activeScope = SCOPE_FAN;         // boot default — safe on a boat
uint8_t  selectedFan = SELECT_ALL;        // 0 = all in scope; 1..5 = specific
bool     fanOn       = false;
int8_t   levelIndex  = 5;                 // 0..10 scale; midpoint at boot (63% duty)
uint8_t  lastCmd     = 0xFF;
uint32_t lastActMs   = 0;
uint32_t lastVolMs   = 0;                 // last accepted VOL step (R2 exception)

// ---------- ESP-NOW receive queue ----------
volatile uint8_t rxBuf[8];
volatile uint8_t rxHead = 0, rxTail = 0;

// ---------------------------------------------------------
uint8_t readFanID() {
  pinMode(ID_PIN_A, INPUT_PULLUP);
  pinMode(ID_PIN_B, INPUT_PULLUP);
  pinMode(ID_PIN_C, INPUT_PULLUP);
  // The pull-up RC constant is a few microseconds; 5 ms is generous but
  // costs nothing at boot and covers any capacitance from long jumper wires.
  delay(5);

  int rawA = digitalRead(ID_PIN_A);
  int rawB = digitalRead(ID_PIN_B);
  int rawC = digitalRead(ID_PIN_C);

#if SERIAL_DEBUG
  Serial.printf("ID pin reads: A(GPIO%u)=%s  B(GPIO%u)=%s  C(GPIO%u)=%s\n",
                (unsigned)ID_PIN_A, rawA == LOW ? "LOW " : "HIGH",
                (unsigned)ID_PIN_B, rawB == LOW ? "LOW " : "HIGH",
                (unsigned)ID_PIN_C, rawC == LOW ? "LOW " : "HIGH");
#endif

  bool a = (rawA == LOW);
  bool b = (rawB == LOW);
  bool c = (rawC == LOW);

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
// Return value is a short label for the Serial log — describes what the node
// actually did (or why it did nothing), so `SERIAL_DEBUG` output makes each
// command byte traceable end-to-end.
//
// Scope keys and select keys are processed unconditionally so every node
// stays in sync with the remote's addressing state. Everything else is
// gated behind (activeScope == THIS_NODE_SCOPE).
const char* applyCommand(uint8_t cmd) {
  // --- Scope selectors (color keys) — always processed. Reset selection. ---
  if (cmd == CMD_SCOPE_FAN)   { activeScope = SCOPE_FAN;   selectedFan = SELECT_ALL; return "SCOPE fan"; }
  if (cmd == CMD_SCOPE_LED)   { activeScope = SCOPE_LED;   selectedFan = SELECT_ALL; return "SCOPE led"; }
  if (cmd == CMD_SCOPE_RED)   { activeScope = SCOPE_RED;   selectedFan = SELECT_ALL; return "SCOPE red"; }
  if (cmd == CMD_SCOPE_GREEN) { activeScope = SCOPE_GREEN; selectedFan = SELECT_ALL; return "SCOPE grn"; }

  // --- Selectors (numeric keys) — always processed, narrows within scope. ---
  for (uint8_t i = 0; i < 5; i++)
    if (cmd == CMD_SELECT[i]) {
      selectedFan = i + 1;
      static const char* labels[5] = {"SEL 1", "SEL 2", "SEL 3", "SEL 4", "SEL 5"};
      return labels[i];
    }
  if (cmd == CMD_SELECT_ALL) { selectedFan = SELECT_ALL; return "SEL all"; }

  // --- Everything below is scope-gated. ---
  if (activeScope != THIS_NODE_SCOPE) return "scope skip";

  bool mine = (selectedFan == SELECT_ALL) || (selectedFan == fanID);

  if (cmd == CMD_VOL_UP) {
    if (!mine) return "SPD+ skip";
    if (levelIndex < NUM_LEVELS - 1) levelIndex++;
    fanOn = true; applyOutput();
    return "SPD+";
  }
  if (cmd == CMD_VOL_DOWN) {
    if (!mine) return "SPD- skip";
    if (levelIndex > 0) levelIndex--;
    fanOn = true; applyOutput();
    return "SPD-";
  }
  if (cmd == CMD_POWER) {
    if (!mine) return "PWR skip";
    fanOn = !fanOn; applyOutput();
    return fanOn ? "PWR on" : "PWR off";
  }
  if (cmd == CMD_OFF) {
    if (!mine) return "OFF skip";
    fanOn = false;
    levelIndex = 0;              // reset — MUTE means "back to zero"
    applyOutput();
    return "OFF";
  }
  return "unmapped";
}

static char scopeChar(uint8_t s) {
  switch (s) {
    case SCOPE_FAN:   return 'F';
    case SCOPE_LED:   return 'L';
    case SCOPE_RED:   return 'R';
    case SCOPE_GREEN: return 'G';
    default:          return '?';
  }
}

static const char* scopeName(uint8_t s) {
  switch (s) {
    case SCOPE_FAN:   return "FAN";
    case SCOPE_LED:   return "LED";
    case SCOPE_RED:   return "RED";
    case SCOPE_GREEN: return "GREEN";
    default:          return "?";
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
  // Set CPU frequency BEFORE bringing up Serial or the IR receiver so
  // baud-rate and IRremote timer calibration land on the final APB clock.
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
#if SERIAL_DEBUG
  Serial.begin(115200); delay(50);
#endif
  fanID = readFanID();
#if SERIAL_DEBUG
  Serial.printf("\nThis board is %s %d\n", scopeName(THIS_NODE_SCOPE), fanID);
  Serial.printf("channel=%d cpu=%uMHz\n", WIFI_CHANNEL, (unsigned)getCpuFrequencyMhz());
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
  // IR in: initial frames + (VOL+/- only) repeat frames — execute + rebroadcast.
  // Every IR event we hear gets a Serial line, so it's obvious whether
  // a keypress reached this node and what the node did with it.
  if (IrReceiver.decode()) {
    auto &d = IrReceiver.decodedIRData;
    if (d.protocol == UNKNOWN) {
#if SERIAL_DEBUG
      Serial.printf("IR  raw=0x%08X UNKNOWN protocol\n",
                    (uint32_t)d.decodedRawData);
#endif
    } else if (d.flags & IRDATA_FLAGS_IS_REPEAT) {
      // R2 exception: VOL+/- accept repeats at ~3 steps/sec while held so a
      // user can ramp without repeated presses. Everything else stays silent.
      uint8_t cmd = d.command;
      bool isVol = (cmd == CMD_VOL_UP) || (cmd == CMD_VOL_DOWN);
      if (isVol && (millis() - lastVolMs) >= VOL_REPEAT_MS) {
        lastVolMs = millis();
        (void)accept(cmd);              // sync R3 dedup so sibling RF copies drop
        signalBlink();
        const char* action = applyCommand(cmd);
        meshBroadcast(cmd);
#if SERIAL_DEBUG
        Serial.printf("IR  0x%02X %-10s scope=%c sel=%d on=%d step=%d (rep)\n",
                      cmd, action, scopeChar(activeScope), selectedFan, fanOn, levelIndex);
#endif
      } else {
#if SERIAL_DEBUG
        Serial.printf("IR  0x%02X REPEAT     (ignored, R2)\n", cmd);
#endif
      }
    } else {
      uint8_t cmd = d.command;
      if (accept(cmd)) {
        signalBlink();
        const char* action = applyCommand(cmd);
        meshBroadcast(cmd);
        if (cmd == CMD_VOL_UP || cmd == CMD_VOL_DOWN) lastVolMs = millis();
#if SERIAL_DEBUG
        Serial.printf("IR  0x%02X %-10s scope=%c sel=%d on=%d step=%d\n",
                      cmd, action, scopeChar(activeScope), selectedFan, fanOn, levelIndex);
#endif
      } else {
#if SERIAL_DEBUG
        Serial.printf("IR  0x%02X DEDUP      (within %u ms of last)\n",
                      cmd, (unsigned)DEDUP_MS);
#endif
      }
    }
    IrReceiver.resume();
  }

  // ESP-NOW in: execute, but NEVER rebroadcast. Dedup rejections here are
  // expected — every other node's rebroadcast lands within R3's window.
  while (rxTail != rxHead) {
    uint8_t cmd = rxBuf[rxTail];
    rxTail = (rxTail + 1) & 7;
    if (accept(cmd)) {
      signalBlink();
      const char* action = applyCommand(cmd);
#if SERIAL_DEBUG
      Serial.printf("RF  0x%02X %-10s scope=%c sel=%d on=%d step=%d\n",
                    cmd, action, scopeChar(activeScope), selectedFan, fanOn, levelIndex);
#endif
    } else {
#if SERIAL_DEBUG
      Serial.printf("RF  0x%02X DEDUP      (within %u ms of last)\n",
                    cmd, (unsigned)DEDUP_MS);
#endif
    }
  }

  blinkUpdate();
  if (SEL_LED_PIN >= 0) {
    bool addressed = (activeScope == THIS_NODE_SCOPE)
                  && (selectedFan == SELECT_ALL || selectedFan == fanID);
    digitalWrite(SEL_LED_PIN, addressed ? HIGH : LOW);
  }
}
