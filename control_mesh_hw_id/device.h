// device.h — interface between the common mesh firmware and the
// device-specific output stage.
//
// Adding a new device class (e.g. LED strips) means creating a new sketch
// directory that reuses control_mesh_hw_id.ino and this header, and
// provides its own device_<class>.cpp implementing the three symbols
// declared below. No changes to the common code are required.
//
// Contract:
//   - THIS_NODE_SCOPE identifies which scope this binary handles. The
//     common code only acts on VOL+/-/POWER/MUTE when the remote's
//     active scope matches this value.
//   - deviceBegin() runs once at boot, before deviceApply(). It must
//     leave the output in a safe/off state.
//   - deviceApply(on, levelIndex) is called whenever the common state
//     changes. levelIndex is in [0, NUM_LEVELS-1]; the implementation
//     maps that 0..10 scale to whatever hardware output it drives
//     (PWM duty for fans, LED brightness, etc.). `on == false` means
//     "output nothing" (fan stopped / LEDs dark), regardless of level.

#pragma once
#include <stdint.h>

// Project-wide compile-time settings. Included via device.h so every module
// can gate its Serial output the same way.
#define SERIAL_DEBUG 1

// The 0..10 user-facing level scale is a universal contract across every
// scope on the mesh. Each device implementation maps it to its own range.
constexpr uint8_t NUM_LEVELS = 11;

// Scope selectors (device classes on the mesh). Every node tracks the
// remote's active scope via color keys; only nodes whose THIS_NODE_SCOPE
// matches act on non-scope/non-select commands.
enum Scope : uint8_t {
  SCOPE_FAN   = 0,
  SCOPE_LED   = 1,
  SCOPE_RED   = 2,
  SCOPE_GREEN = 3,
};

// Defined by the single device_*.cpp compiled into this binary.
extern const uint8_t THIS_NODE_SCOPE;

// Configure output hardware. Called once at boot. Must leave the output
// in a safe/off state before returning.
void deviceBegin();

// Translate common state (on + level 0..10) to hardware output.
// levelIndex is guaranteed to be in [0, NUM_LEVELS-1].
void deviceApply(bool on, uint8_t levelIndex);
