# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project context

DIY controller for 5× 12V 4-pin PWM fans (Arctic P14 Pro PST) on a sailboat, driven by a standard IR remote, with five identical ESP32 nodes forming a wireless mesh via ESP-NOW. No hub, no wiring between nodes, no per-board code changes — node identity comes from hardware jumpers read at boot.

**Read `docs/HANDOVER.md` first.** It is the canonical context document and contains all architectural rationale, hardware decisions, accepted trade-offs, and the validated test-on-hardware roll-out plan. The repo has no live deployment yet — nothing has been tested on physical hardware.

`docs/HANDOVER.md` is written in Danish — keep it that way when editing. Code, code comments, and other docs are in English.

## Files

- `control_mesh_hw_id/control_mesh_hw_id.ino` — the active firmware. All five nodes flash this identical binary.
- `ir_diagnostic/ir_diagnostic.ino` — standalone diagnostic sketch to identify the protocol/codes from an unknown IR remote before plugging them into the mesh firmware's `CMD_*` constants.

Each is a standalone Arduino sketch (the `.ino` filename matches its parent directory, as Arduino requires). They are not a multi-file build — each is flashed on its own.

## Build / flash

No automated build, lint, or test exists — this is an Arduino project.

- Toolchain: Arduino IDE *or* `arduino-cli`, ESP32 Arduino core (2.x or 3.x — the firmware compiles for both via `#if ESP_ARDUINO_VERSION_MAJOR >= 3`), and the **IRremote** library v4.x.
- Board: a plain ESP32 dev board (no PSRAM — see GPIO conflict note below).
- Serial monitor: 115200 baud.
- All nodes must use the same Wi-Fi channel (`WIFI_CHANNEL = 1`).

## Invariants — do not break without explicit user agreement

These three rules together keep the mesh stable. They are implemented in `loop()` and `accept()` and explained in HANDOVER.md.

- **R1 — single hop.** Only IR-received commands are rebroadcast via ESP-NOW. Commands received via ESP-NOW are executed but NEVER forwarded. Prevents broadcast storms.
- **R2 — no repeat frames.** NEC repeat frames are ignored entirely. Each physical keypress is one discrete step. Holding a key does not ramp — this is intentional, not a bug.
- **R3 — 150 ms dedup window.** The same command within 150 ms of the last accepted one is dropped, because several nodes typically hear the same IR pulse simultaneously.

Other deliberate choices that look like omissions but are not:
- **No NEC `address` filtering** — only the `command` byte is matched. Interference risk judged acceptable.
- **Partial boot-window failsafe.** Fans may briefly run full speed (<1 s) during boot before LEDC initialises. The user has accepted this and explicitly rejected a MOSFET hard-cutoff. The 10 kΩ pullup on `PWM_PIN` to 3.3V plus `digitalWrite(PWM_PIN, LOW)` in `pwmBegin()` is the agreed mitigation.
- **NEC-only decode** (`#define DECODE_NEC`). If the final remote turns out not to be NEC, prefer changing the remote's device code over expanding the decoder.
- **Placeholder IR command bytes.** `CMD_SELECT[]`, `CMD_VOL_UP`, etc. in the mesh firmware are placeholder values from a generic NEC remote. They must be replaced with values captured by `ir_diagnostic.ino` once the final remote is chosen.

## Hardware notes that affect code

- **GPIO conflict on PSRAM boards.** ID-pins (16/17/18) collide with PSRAM SPI on WROVER variants. If switching to a PSRAM board, move ID pins (HANDOVER suggests GPIO 32/33/34 — note 34+ are input-only without internal pullup and need external 10 kΩ).
- **LEDC API differs between ESP32 core 2.x and 3.x.** `pwmBegin()` / `pwmWrite()` already branch on `ESP_ARDUINO_VERSION_MAJOR`. Preserve both branches in any PWM-related edit.
- **PWM frequency is 25 kHz** (Intel 4-pin PWM fan spec). Do not change.

## Deferred work — do not build proactively

HANDOVER.md describes a future LED-strip extension using the remote's color keys as device-type selectors. The user has explicitly asked that this stay on ice until they bring it up again. Do not start sketching it in code or comments uninvited.
