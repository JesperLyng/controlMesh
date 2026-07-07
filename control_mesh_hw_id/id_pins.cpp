// id_pins.cpp — jumper-based node ID implementation.

#include <Arduino.h>
#include "device.h"    // SERIAL_DEBUG
#include "id_pins.h"

static const uint8_t ID_PIN_A = 32;
static const uint8_t ID_PIN_B = 33;
static const uint8_t ID_PIN_C = 25;

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
