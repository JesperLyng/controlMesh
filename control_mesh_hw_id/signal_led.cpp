// signal_led.cpp — signal LED implementation.

#include <Arduino.h>
#include "signal_led.h"

static const uint8_t  SIG_LED_PIN = 19;   // 330 Ohm in series
static const uint16_t BLINK_MS    = 50;
static uint32_t blinkOffAt = 0;

void signalLedBegin() {
  pinMode(SIG_LED_PIN, OUTPUT);
  digitalWrite(SIG_LED_PIN, LOW);
}

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
