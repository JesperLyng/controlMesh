/*
 * IR diagnostics — find the protocol and codes from your remote
 * ----------------------------------------------------------------
 * Purpose: after setting the URC1210 to a device code (e.g. TV/Aiwa 4542),
 * use this sketch to see which PROTOCOL it actually transmits and
 * which command value each key produces.
 *
 * It decodes ALL protocols IRremote supports — not just Samsung — so you
 * can immediately tell whether the device code is compatible with the
 * simple mesh firmware (which currently only understands Samsung).
 *
 * Usage:
 *   1. Flash this sketch, open Serial Monitor at 115200 baud.
 *   2. Press every key you plan to use (1-5, +, -, power, 0).
 *   3. Read the line for each: protocol, address, command.
 *   4. If the protocol is Samsung -> use the 'command' value directly
 *      in CMD_SELECT[] etc. in the mesh firmware.
 *   5. If the protocol is anything else (NEC, RC5, Sony, etc.) -> try
 *      the next device code on the list for your brand and test again.
 *
 * Requires: "IRremote" library v4.x.
 */

#define DECODE_SAMSUNG
#include <IRremote.hpp>

const uint8_t IR_PIN = 4;   // same pin as in the mesh firmware

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("=== IR diagnostics ready ==="));
  Serial.println(F("Press keys on the remote ..."));
  IrReceiver.begin(IR_PIN, ENABLE_LED_FEEDBACK);
}

void loop() {
  if (IrReceiver.decode()) {
    auto &d = IrReceiver.decodedIRData;

    if (d.flags & IRDATA_FLAGS_IS_REPEAT) {
      Serial.println(F("  (repeat frame - normally ignored)"));
    } else {
      Serial.print(F("protocol="));
      Serial.print(getProtocolString(d.protocol));
      Serial.print(F("  address=0x"));
      Serial.print(d.address, HEX);
      Serial.print(F("  command=0x"));
      Serial.print(d.command, HEX);
      Serial.print(F("  raw=0x"));
      Serial.println((uint32_t)d.decodedRawData, HEX);

      if (d.protocol == UNKNOWN) {
        Serial.println(F("  -> UNKNOWN: could not be decoded as any known protocol."));
        Serial.println(F("     Try a different device code on the remote."));
      } else if (d.protocol != SAMSUNG) {
        Serial.println(F("  -> Not Samsung. Either extend the mesh firmware or change the device code."));
      }
    }

    IrReceiver.resume();
  }
}
