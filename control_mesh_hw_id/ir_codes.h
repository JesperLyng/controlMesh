// ir_codes.h — IR command byte constants (Samsung protocol, address 0x0E).
//
// Captured from the URC1210 remote with ir_diagnostic.ino. Codes 0x06..0x09
// (keys 6-9) are captured but unmapped — reserved for future CMD_SELECT
// expansion or another device class on the same mesh.
//
// The Scope enum itself lives in device.h; the CMD_SCOPE_* codes here are
// the Samsung command bytes that map to each scope selector key.

#pragma once
#include <stdint.h>

// Selectors (processed unconditionally on every node — the shared selection
// state must stay coherent even when a node is not in the active scope).
constexpr uint8_t CMD_SELECT[5]    = {0x01, 0x02, 0x03, 0x04, 0x05};
constexpr uint8_t CMD_SELECT_ALL   = 0x00;   // key "0"

// Actions — only fire when the remote's active scope matches THIS_NODE_SCOPE.
constexpr uint8_t CMD_VOL_UP       = 0x14;
constexpr uint8_t CMD_VOL_DOWN     = 0x15;
constexpr uint8_t CMD_POWER        = 0x0C;   // toggle the current selection
constexpr uint8_t CMD_OFF          = 0x0D;   // MUTE: toggle mute/unmute

// Scope selectors (color keys) — set activeScope AND reset selection to all.
constexpr uint8_t CMD_SCOPE_RED    = 0xA0;
constexpr uint8_t CMD_SCOPE_GREEN  = 0xA1;
constexpr uint8_t CMD_SCOPE_LED    = 0xA2;   // YELLOW
constexpr uint8_t CMD_SCOPE_FAN    = 0xA3;   // BLUE
