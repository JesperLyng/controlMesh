/*
 * Fan controller (ESP32) — IR + ESP-NOW mesh
 * -------------------------------------------------
 * All five nodes are IDENTICAL — no code changes per board.
 * NODE_ID (1-5) is set with jumpers to GND — see the truth table in id_pins.h.
 *
 * This .ino is the sketch entry point plus the pieces that touch the shared
 * state directly: state variables, applyCommand's dispatch, and setup/loop.
 * The rest lives in focused modules so a new device class only touches the
 * output stage:
 *
 *   device.h / device_fan.cpp — output stage interface + fan implementation
 *   ir_codes.h                — CMD_* command bytes from the remote
 *   mesh.h  / mesh.cpp        — ESP-NOW transport + R3 dedup
 *   signal_led.h / .cpp       — non-blocking signal LED
 *   id_pins.h / id_pins.cpp   — jumper-based node ID
 *
 * See device.h for the contract a new class (e.g. LEDs) must implement.
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
 *   MUTE     : toggle. First press bookmarks the current state and starts
 *              a mute-fade: fan drops to level 0 (25% duty) to brake the
 *              motor, then cuts to 0% after MUTE_FADE_MS. Second press
 *              restores the bookmark (unmute) — if pressed during the
 *              fade, the fade is aborted mid-brake. VOL+/-/POWER on this
 *              fan also abort a pending fade and clear the bookmark. At
 *              boot the bookmark is pre-populated with "fan on at default
 *              level" so the first MUTE press after power-on acts as a
 *              one-button start.
 *
 * Commands other than scope/select are only acted on when the active scope
 * matches THIS_NODE_SCOPE — so pressing YELLOW then MUTE is a no-op on fan
 * nodes but will kill all LEDs once LED nodes exist.
 *
 * Requires: ESP32 Arduino core (2.x or 3.x) + "IRremote" library (v4.x).
 */

#define DECODE_SAMSUNG
#include <IRremote.hpp>

#include "device.h"
#include "ir_codes.h"
#include "mesh.h"
#include "signal_led.h"
#include "id_pins.h"

// ---------- Pins that only setup()/loop() touch directly ----------
const uint8_t IR_PIN      = 13;
const int8_t  SEL_LED_PIN = -1;  // lit when this node is addressable, or -1 to disable

// ---------- CPU frequency ----------
// 80 MHz is the lowest ESP32 setting that still keeps WiFi/ESP-NOW alive
// (40 MHz and below disable the radio). APB clock stays at 80 MHz here too,
// so LEDC PWM timing and Serial are unaffected, and IRremote's timers
// re-scale automatically. Halves idle current vs the 240 MHz default —
// matters on a boat. Do not lower further without an alternative mesh transport.
const uint32_t CPU_FREQ_MHZ = 80;

// ---------- UX timing ----------
// R2 exception: VOL+/- accept repeat frames while held, capped at ~3 steps/sec.
const uint16_t VOL_REPEAT_MS = 333;

// MUTE fade: run the fan at level 0 (25% duty) for this long before cutting
// to full 0%. The brief low-duty phase brakes the motor actively so the fan
// spins down faster than it would just coasting from high RPM.
const uint16_t MUTE_FADE_MS  = 3000;

// Sentinel: selectedFan == SELECT_ALL means "apply to every node in the active scope".
const uint8_t SELECT_ALL = 0;

// ---------- State ----------
uint8_t  fanID       = 1;                 // read in setup()
uint8_t  activeScope = SCOPE_FAN;         // boot default — safe on a boat
uint8_t  selectedFan = SELECT_ALL;        // 0 = all in scope; 1..5 = specific
bool     fanOn       = false;
int8_t   levelIndex  = 2;                 // 0..10 scale at boot
uint32_t lastVolMs   = 0;                 // last accepted VOL step (R2 exception)

// MUTE toggle state — a MUTE press bookmarks (fanOn, levelIndex) so a second
// MUTE press restores them. Any VOL+/-, POWER (or another OFF-cycle) clears
// the bookmark, since the user has actively changed the state.
//
// Boot state: muted=true with the bookmark pre-populated as "fan on at
// default level", so the first MUTE press after power-on unmutes into the
// running state — a one-button "start". Any other command (VOL+, POWER)
// still works normally at boot and clears the bookmark as usual.
bool     muted       = true;
bool     savedFanOn  = true;
int8_t   savedLevel  = 5;

// Non-blocking mute-fade completion. First MUTE press runs the fan at level 0
// for MUTE_FADE_MS to brake it actively, then loop() cuts to 0% duty. Value 0
// means "no pending completion". Any VOL+/- or POWER on this fan aborts it.
uint32_t muteFinishAt = 0;

// ---------- Output ----------
// Thin wrapper so the command-logic call sites don't need to know about
// the device-layer interface. The actual output stage lives in device_*.cpp.
void applyOutput() { deviceApply(fanOn, levelIndex); }

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
    muted = false;               // active change clears the MUTE bookmark
    muteFinishAt = 0;            // abort any pending MUTE fade
    if (levelIndex < NUM_LEVELS - 1) levelIndex++;
    fanOn = true; applyOutput();
    return "SPD+";
  }
  if (cmd == CMD_VOL_DOWN) {
    if (!mine) return "SPD- skip";
    muted = false;
    muteFinishAt = 0;
    if (levelIndex > 0) levelIndex--;
    fanOn = true; applyOutput();
    return "SPD-";
  }
  if (cmd == CMD_POWER) {
    if (!mine) return "PWR skip";
    muted = false;
    muteFinishAt = 0;
    fanOn = !fanOn; applyOutput();
    return fanOn ? "PWR on" : "PWR off";
  }
  if (cmd == CMD_OFF) {
    if (!mine) return "OFF skip";
    if (muted) {
      // Second MUTE press: restore whatever was saved (also aborts a
      // still-pending fade if the user double-taps within MUTE_FADE_MS).
      fanOn      = savedFanOn;
      levelIndex = savedLevel;
      muted      = false;
      muteFinishAt = 0;
      applyOutput();
      return "UNMUTE";
    }
    // First MUTE press: bookmark, ramp down to lowest level (25% duty) to
    // brake the fan, and schedule the final cut to 0% in MUTE_FADE_MS —
    // loop() finishes the mute so we don't block IR/mesh here.
    savedFanOn = fanOn;
    savedLevel = levelIndex;
    fanOn      = true;
    levelIndex = 0;
    muted      = true;
    muteFinishAt = millis() + MUTE_FADE_MS;
    applyOutput();
    return "MUTE lo";
  }
  return "unmapped";
}

// ---------- Scope helpers (for logging + boot banner) ----------
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
  Serial.printf("channel=%d cpu=%uMHz\n", (int)WIFI_CHANNEL, (unsigned)getCpuFrequencyMhz());
#endif
  signalLedBegin();
  if (SEL_LED_PIN >= 0) { pinMode(SEL_LED_PIN, OUTPUT); digitalWrite(SEL_LED_PIN, LOW); }
  deviceBegin();
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
        (void)dedupAccept(cmd);         // sync R3 dedup so sibling RF copies drop
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
      if (dedupAccept(cmd)) {
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
  uint8_t cmd;
  while (meshPoll(&cmd)) {
    if (dedupAccept(cmd)) {
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

  // Complete a scheduled MUTE fade — see CMD_OFF's first-press branch.
  if (muteFinishAt != 0 && millis() >= muteFinishAt) {
    fanOn = false;
    applyOutput();
    muteFinishAt = 0;
#if SERIAL_DEBUG
    Serial.printf("--  MUTE cut    scope=%c sel=%d on=%d step=%d\n",
                  scopeChar(activeScope), selectedFan, fanOn, levelIndex);
#endif
  }

  blinkUpdate();
  if (SEL_LED_PIN >= 0) {
    bool addressed = (activeScope == THIS_NODE_SCOPE)
                  && (selectedFan == SELECT_ALL || selectedFan == fanID);
    digitalWrite(SEL_LED_PIN, addressed ? HIGH : LOW);
  }
}
