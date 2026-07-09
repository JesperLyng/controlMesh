// device_fan.cpp — fan output stage for the mesh firmware.
//
// Drives an Arctic P14 Pro PST (or any 4-pin 25 kHz PWM fan) directly from
// PWM_PIN via LEDC. See device.h for the interface contract.

#include <Arduino.h>
#include "device.h"

// Compile-time class identifier — this binary is a fan node.
const uint8_t THIS_NODE_SCOPE = SCOPE_FAN;

// ---------- Fan PWM output ----------
// PWM_PIN is GPIO 4 — a plain general-purpose GPIO with no boot-strap or
// shared-peripheral duties. Any LEDC-capable GPIO works; this one is chosen
// to fit the physical layout of the node (IR receiver is on GPIO 13).
// PWM_FREQ is 25 kHz per the Intel 4-pin PWM fan spec — do not change.
// PWM_RES = 10 (0..1023 range) also matches the reference; 8-bit at 25 kHz
// works in theory but is less battle-tested on Arduino ESP32 core 3.x.
// PWM_CH must not collide with any other LEDC channel in this binary; we
// only use one, and IRremote v4.x's receiver doesn't touch LEDC.
static const uint8_t  PWM_PIN  = 4;
static const uint32_t PWM_FREQ = 25000;
static const uint8_t  PWM_RES  = 8;
static const uint8_t  PWM_CH   = 0;

// Level -> duty mapping for the 0..10 scale. Linear from 25% (level 0) to
// 100% (level 10), ~7.5% per step, rounded. 25% may be below the fan's
// stall floor — needs hardware verification that the actual fans start
// reliably from level 0 or 1.
static const uint8_t LEVELS[NUM_LEVELS] = {25, 33, 40, 48, 55, 63, 70, 78, 85, 93, 100};

static void pwmWrite(uint8_t percent) {
  if (percent > 100) percent = 100;
  uint32_t duty = (uint32_t)percent * ((1u << PWM_RES) - 1) / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PWM_PIN, duty);   // 3.x API takes the pin
#else
  ledcWrite(PWM_CH, duty);    // 2.x API takes the channel
#endif
}

void deviceBegin() {
  // Hand the pin to LEDC directly and initialise it to 0 duty. Match the
  // working reference (Ableton fan controller) on this hardware: no
  // pinMode preamble, and an explicit ledcWrite(0) right after attach so
  // the channel state is defined before anything else touches it.
  //
  // Core 2.x uses channel-based setup + attachPin; core 3.x collapses that
  // into a single ledcAttach(pin, freq, res) call. Both APIs are supported
  // via the version guard — Arduino ESP32 core 3.x removed the 2.x names
  // entirely, so the conditional is compile-time required, not cosmetic.
  //
  // The fan may briefly run at full speed during boot before this runs —
  // an unconfigured GPIO reads as floating on the fan's PWM input, which
  // is treated as 100% duty (the safety default for PC cooling). Accepted
  // trade-off; the window closes as soon as LEDC starts driving 0% below.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PWM_PIN, PWM_FREQ, PWM_RES);
  ledcWrite(PWM_PIN, 0);
#else
  ledcSetup(PWM_CH, PWM_FREQ, PWM_RES);
  ledcAttachPin(PWM_PIN, PWM_CH);
  ledcWrite(PWM_CH, 0);
#endif
}

void deviceApply(bool on, uint8_t levelIndex) {
  pwmWrite(on ? LEVELS[levelIndex] : 0);
}
