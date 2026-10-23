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

- **v0.1.0** (first tagged release): `log_frames` now defaults to `false`; the pairing PIN is no longer
  written to the logs; reconnect timing stays correct after ~49.7 days of uptime (millis() wrap); boards
  without classic Bluetooth are rejected at validation; GitHub Actions compiles the example on every push.

To avoid `main` changing underneath you, pin the component to a
[release](https://github.com/scottbambrick/esphome-skyrc-nc2500/releases):
`source: github://scottbambrick/esphome-skyrc-nc2500@v0.1.0`.

## Status
Working on real hardware (NC2500, firmware as of Oct 2026):
- Reading all four slots.
- **Charge** and **Refresh** modes have been set and run on real cells (a complete Refresh).
- **Break-In**, **Discharge** and **Cycle** are decoded from the vendor app but **not tested on a real battery**.
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

## Protocol
[`docs/protocol.md`](docs/protocol.md) describes the full stack (link, module, adapter, session, charger layers).

## Disclaimer
Unofficial, not affiliated with SkyRC. You are charging batteries: use at your own risk.
