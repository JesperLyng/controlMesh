// id_pins.h — hardware node identification via three GPIO jumpers.
//
// Every board flashes the same binary; each node identifies itself as
// 1..5 by reading three GPIO pins in readFanID(). The pins are configured
// as INPUT_PULLUP, so the ESP32 holds each one at 3.3V through an internal
// ~45 kOhm resistor. A pin with nothing attached reads HIGH; a pin
// jumpered to GND wins that pull-down fight and reads LOW. No external
// components are needed — just three wires and up to two jumpers per board.
//
// Three pins = 3 bits = 8 patterns; only 5 are mapped to IDs. The
// remaining 3 patterns fall back to ID 1 with a Serial warning, so a
// wiring mistake still produces a working (if duplicate) node. Mapping
// is arbitrary but stable — pick which physical board is which by
// fitting jumpers before power-up.
//
// Truth table (— = float / no jumper, GND = jumper to GND):
//
//   ID_PIN_A  ID_PIN_B  ID_PIN_C   -> ID
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

#pragma once
#include <stdint.h>

// Read the node's ID (1..5) from the jumper pins. Logs raw HIGH/LOW per
// pin to Serial (when SERIAL_DEBUG) so a mis-mapped jumper reveals itself.
uint8_t readFanID();
