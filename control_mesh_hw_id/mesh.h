// mesh.h — ESP-NOW mesh transport + R3 dedup.
//
// The mesh is broadcast-only over a fixed Wi-Fi channel (see mesh.cpp for
// the channel constant — all nodes MUST agree). Every command that gets
// accepted by dedupAccept() is a candidate to broadcast; commands received
// via the mesh execute locally but are never rebroadcast (R1: single hop).

#pragma once
#include <stdint.h>

// Wi-Fi channel used by the mesh. All nodes MUST use the same value; a
// mismatched board is invisible to the others until reflashed.
constexpr uint8_t WIFI_CHANNEL = 1;

// R3 — same command byte within DEDUP_MS of the last accepted one is
// dropped. Prevents double-execution when several nodes hear the same IR
// pulse and rebroadcast, and when a rebroadcast lands back on the source.
constexpr uint16_t DEDUP_MS = 150;

// Bring up Wi-Fi in STA mode and register the ESP-NOW receiver. Must be
// called after Serial.begin() so any error is logged.
void meshBegin();

// Broadcast a command byte to every other node on the mesh.
void meshBroadcast(uint8_t cmd);

// Non-blocking poll for a command received over the mesh. Returns true and
// stores the command byte in *cmd if one was available, false otherwise.
bool meshPoll(uint8_t* cmd);

// R3 gate: returns true if this command should be processed, false if it
// falls within the dedup window. Call once per candidate command (IR or
// mesh source) — the dedup state is shared across both paths.
bool dedupAccept(uint8_t cmd);
