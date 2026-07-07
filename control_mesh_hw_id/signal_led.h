// signal_led.h — non-blocking signal LED that blinks briefly whenever a
// valid IR/mesh command is accepted, for field debugging of IR coverage.

#pragma once

// Configure the signal-LED pin as OUTPUT and drive it low. Call once from setup().
void signalLedBegin();

// Turn the LED on and schedule it to go off after BLINK_MS. Call from any
// context that has just accepted a command.
void signalBlink();

// Non-blocking: turn the LED off once its scheduled off-time has elapsed.
// Call from loop() every iteration.
void blinkUpdate();
