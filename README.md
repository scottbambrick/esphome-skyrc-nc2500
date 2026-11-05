# esphome-skyrc-nc2500

ESPHome external component for the **SkyRC NC2500** 4-slot NiMH charger. It talks to the charger over
**classic Bluetooth (SPP)** from an **original ESP32** and exposes each slot to Home Assistant: voltage,
current, capacity (mAh), temperature, run time, internal resistance, cycles, mode, state and battery
presence. You can also set the mode (Charge / Refresh / Break-In / Discharge / Cycle) and its currents and
start it from Home Assistant.

The NC2500 is not BLE, so Home Assistant's Bluetooth integration and ESPHome Bluetooth proxies cannot see it,
and the current "SkyRC" phone app doesn't support it (the old *SkyRC Smart Charger* app does).

## Requirements
- An **original ESP32** (classic Bluetooth). S2/S3/C3/C6 cannot do this.
- ESPHome 2026.9 or newer with the ESP-IDF framework. Don't enable `esp32_ble` / `bluetooth_proxy` on the node.
  (Choosing an S2/S3/C3/C6 board is rejected when the config is validated.)
- The charger switched on for Bluetooth (hold ENTER until the blue LED lights). It only answers while on.

## Use
See [`example.yaml`](example.yaml). Minimum:

```yaml
external_components:
  - source: github://scottbambrick/esphome-skyrc-nc2500
    components: [skyrc_nc2500]

skyrc_nc2500:
  id: charger
  pin: "1234"          # try "0000" if pairing fails
  slots:
    - voltage: { name: "Slot 1 Voltage" }
```

On the first run leave `mac_address` unset: the ESP32 searches for a device whose name contains `skyrc`
and logs what it finds. Copy the address into `mac_address` (and `channel: 2`) so later connections go
straight to the charger.

The charger accepts one Bluetooth connection at a time. The example has a "Bluetooth Link" switch to let go of
it for the phone app.

## Changes

- **v0.2.0**: `stop_slot()` and a "Slot N Stop" button per slot in the example (a set-slot with mode 5 ends
  the slot's program); Discharge is now tested on hardware; protocol notes corrected (present byte, state
  codes). Thanks to [@rpcyan586](https://github.com/rpcyan586) for both.
- **v0.1.0** (first tagged release): `log_frames` now defaults to `false`; the pairing PIN is no longer
  written to the logs; reconnect timing stays correct after ~49.7 days of uptime (millis() wrap); boards
  without classic Bluetooth are rejected at validation; GitHub Actions compiles the example on every push.

To avoid `main` changing underneath you, pin the component to a
[release](https://github.com/scottbambrick/esphome-skyrc-nc2500/releases):
`source: github://scottbambrick/esphome-skyrc-nc2500@v0.2.0`.

## Status
Working on real hardware (NC2500, firmware as of Oct 2026):
- Reading all four slots.
- **Charge**, **Refresh** and **Discharge** have been set and run on real cells (a complete Refresh and a complete
  Discharge).
- **Break-In** and **Cycle** have been run to completion on NiMH AA cells (see Observed behaviour).
- **Stop**: the vendor app has no stop command, but a set-slot with mode 5 (Done) ends a slot's program. The
  example has a "Slot N Stop" button for each slot.
- System settings (delta-V, cut-offs) and firmware update commands are deliberately not implemented.

`home-assistant/progress-estimates-package.yaml` is an optional Home Assistant package that estimates charge %
and time left per slot (the charger reports mAh and run time, not how full a battery is). It assumes entity ids
from a node called `office-battery-charger` - adjust to yours.

## Observed behaviour
- **Charge** at 0.2 A stops at the charger's own 300-minute timer (about 1000 mAh), not on delta-V.
- **Refresh** (0.5 A charge, 0.3 A discharge): charge 5 h, rest 1 h, discharge, rest 1 h, charge 5 h, then Done.
  The reported run time **restarts at 0 in every phase**, so it is not the elapsed time of the whole program.
  In one run on already-full cells the discharge phase lasted only ~15 s (the voltage read 0.12 V under load).
  The mAh counter restarts with each charge phase and ended at 2499 mAh (5 h x 0.5 A).
- **Discharge** (0.5 A, from a full cell): discharged to the cut-off voltage (0.90 V), then Done: 1704 mAh in
  3 h 24 min. The mAh counter counts the discharged amount. The slot does not charge afterwards.
- **Break-In** (rated capacity C; tried at 500, 800, 1000 and 2000 mAh): charge at C/10 for 16 h (1.6 x C), rest
  1 h, discharge at C/5 to the cut-off, rest 1 h, charge at C/10 for 16 h, then Done: 34-39 h in all. The charges
  ran the full 16 h every time. The charge current goes below the 200 mA set-slot minimum (50 mA at 500 mAh). A
  cell already at the cut-off gets a discharge of under 20 s, and the program carries on.
- **Cycle** (N cycles; seen with N = 2): charge, rest 1 h, discharge to the cut-off, rest 1 h, N times, then a
  final charge and Done. The cycle number reads 1, then 2 from the second charge. The charges end on their own:
  on healthy 1900 mAh AA cells at 0.5 A and 1 A they stopped at 103-109 % of the discharge before, in 96-232 min.
  On weak cells a charge can run to the 300-minute timer (2499 mAh at 0.5 A); the program then carries on. On a
  full cell the first charge stops within 20 s.

## Protocol
[`docs/protocol.md`](docs/protocol.md) describes the full stack (link, module, adapter, session, charger layers).

## Disclaimer
Unofficial, not affiliated with SkyRC. You are charging batteries: use at your own risk.
