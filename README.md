# controlMesh

A small ESP32 firmware platform for controlling 12V devices on a boat from a
single IR remote, using an ESP-NOW mesh between identical nodes — no hub, no
wiring between devices, and the same binary flashed to every board (each node
reads its own ID from three jumper pins at boot).

Any node that hears an IR signal executes the command locally and rebroadcasts
it to the rest of the mesh, so a single keypress reaches every node even if
line of sight is partly blocked by a bulkhead.

**Currently implemented:** 5× 12V 4-pin PWM fan node (Arctic P14 Pro PST).
**Planned:** other device classes on the same mesh — LED strips first, then
likely pumps, cockpit lighting, etc. The mesh layer (IR receive, ESP-NOW
distribution, dedup rules) is device-agnostic and reused unchanged; each new
device class only needs its own `applyCommand` / output stage. Device-type
selection is intended to use the remote's color keys as a prefix so the 1–5 /
+ / − / power keys remain "local" to the active device class.

> **Status:** firmware-complete for fans but **not yet tested on physical
> hardware**. IR command bytes in `control_mesh_hw_id.ino` are placeholders
> until the final remote is chosen and decoded with `ir_diagnostic.ino`.

## Repository layout

| Path | Purpose |
| --- | --- |
| `control_mesh_hw_id/` | Active firmware for the fan node. Common code (`.ino`, `mesh`, `ir_codes`, `signal_led`, `id_pins`) is device-agnostic; `device_fan.cpp` is the fan-specific output stage. `device.h` declares the interface a new class must implement to share the mesh. Same binary flashes to all five nodes. |
| `ir_diagnostic/` | Standalone sketch that decodes any IR protocol IRremote supports — use it to capture the `command` bytes from your chosen remote before plugging them into a node's firmware. |
| `docs/HANDOVER.md` | Full design rationale, hardware decisions, accepted trade-offs, and the hardware-test roll-out plan (in Danish). Also covers the planned multi-device extension. |
| `CLAUDE.md` | Guidance for the Claude Code AI assistant when working in this repo. |

## Hardware (per fan node, ×5)

- ESP32 dev board (no PSRAM — see [docs/HANDOVER.md](docs/HANDOVER.md) for the
  GPIO 16/17 conflict on WROVER variants)
- Arctic P14 Pro PST 12V 4-pin PWM fan
- IR receiver (VS1838B or TSOP38238, 38 kHz) with local decoupling
- MP1584 buck converter (12V → 3.3V/5V)
- Signal LED + 330 Ω series resistor
- 3× jumpers on the ID pins
- 12V supply with inline 1–2A fuse

Future device classes (LED strips, etc.) will share the ESP32 + IR + buck
converter + ID-jumper recipe and differ only in the output stage.

## Build & flash

1. Install the **ESP32 Arduino core** (2.x or 3.x — the firmware compiles for
   both) and the **IRremote** library v4.x.
2. Open `control_mesh_hw_id/control_mesh_hw_id.ino` in the Arduino IDE
   (or use `arduino-cli`).
3. For each board, fit the ID jumpers per the table at the top of the sketch,
   then flash. All five fan boards get the same binary.
4. Serial monitor: 115200 baud.

All nodes on the mesh must use the same Wi-Fi channel (`WIFI_CHANNEL`,
default `1`).

## Remote keys

```
Scope (color keys — every node tracks scope; also resets selection to "all"):
  BLUE   : fan scope   (this binary)
  YELLOW : LED scope   (planned — a future LED binary will respond)
  RED    : reserved for a future device class
  GREEN  : reserved for a future device class

Selection (within the active scope):
  Keys 1–5 : select a specific instance
  0        : select ALL instances

Action (only fires on nodes whose class matches the active scope):
  + / -    : one step up/down on the current selection
  POWER    : toggle the current selection on/off
  MUTE     : toggle — first press bookmarks state then forces off with
             level reset to 0; second press restores the bookmark. At
             boot the bookmark is pre-populated with "fan on at default
             level", so the first MUTE after power-on acts as a one-
             button start. Any VOL+/-, POWER in between clears the
             bookmark.
```

The scope prefix is sticky — pressing e.g. YELLOW followed by MUTE is
"turn off all LEDs" without an explicit `0`, because pressing a color
also resets the selection to "all in scope".

Level scale is **0–10** across every scope (eleven discrete steps); each
device class maps that range to its own output. Fan node: linear from
25 % duty at level 0 to 100 % at level 10 (7.5 %-per-step), boot value
level 5 (63 %). Whether the fans start reliably at 25 % duty needs
hardware verification.

Holding VOL+ or VOL− ramps at ~3 steps per second while held; every
other command remains one-per-keypress.

Keys `6`–`9` are captured but unmapped — reserved for future
`CMD_SELECT` expansion.

## Mesh rules

Three invariants keep the mesh stable regardless of device class — see
HANDOVER.md and the comments in `loop()` / `accept()` for the full reasoning:

- **R1 — single hop.** Only IR-received commands are rebroadcast over ESP-NOW.
  Commands received from the mesh are executed but never forwarded. No
  broadcast storms.
- **R2 — no repeat frames.** NEC repeat frames (held key) are ignored. Each
  physical press is one discrete step.
- **R3 — 150 ms dedup window.** The same command within 150 ms of the last
  accepted one is dropped, since several nodes typically hear the same IR
  pulse simultaneously.
