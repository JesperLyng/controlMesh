// device_fan.cpp — fan output stage for the mesh firmware.
//
// Drives an Arctic P14 Pro PST (or any 4-pin 25 kHz PWM fan) directly from
// PWM_PIN via LEDC. See device.h for the interface contract.

#include <Arduino.h>
#include "device.h"

// Compile-time class identifier — this binary is a fan node.
const uint8_t THIS_NODE_SCOPE = SCOPE_FAN;

// ---------- Fan PWM output ----------
// PWM_PIN needs a 10 kOhm pullup to 3.3V for the boot-window failsafe
// (see deviceBegin() comment). PWM_FREQ is 25 kHz per the Intel 4-pin
// PWM fan spec — do not change.
static const uint8_t  PWM_PIN  = 5;
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
  ledcWrite(PWM_PIN, duty);
#else
  ledcWrite(PWM_CH, duty);
#endif
}

void deviceBegin() {
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

void deviceApply(bool on, uint8_t levelIndex) {
  pwmWrite(on ? LEVELS[levelIndex] : 0);
}
