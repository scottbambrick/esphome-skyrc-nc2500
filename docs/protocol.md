# SkyRC NC2500 Bluetooth protocol

How the NC2500 talks over classic Bluetooth, as implemented in `components/skyrc_nc2500`. It was worked out by
analysing the old *SkyRC Smart Charger* Android app (com.revogi.smartcharge 0.9.9; the current "SkyRC" app does
not support this charger) and then checked against real hardware. Section 6 lists what has and hasn't been
verified on a real charger.

## 1. Transport
- Classic Bluetooth, RFCOMM serial port (SPP, UUID `00001101-0000-1000-8000-00805F9B34FB`), **not BLE**.
- The charger advertises as `skyrcNNNN` and uses RFCOMM **channel 2**. Pairing PIN `1234`, with no prompt.
- It only answers while its Bluetooth is on (hold ENTER until the blue LED lights) and accepts **one connection at a time**.
- Inside the charger is an "iBT-02" serial bridge module, so the stack has several layers.

## 2. Layers, bottom to top

| # | Layer | Format |
|---|-------|--------|
| 1 | Link | `55`, length, payload, checksum. Length is one byte (up to 252), otherwise `00 hi lo`. Checksum = two's complement of the sum of the length bytes and payload. |
| 2 | Module | `00`, cmd, seq (2 bytes), data. Messages are numbered and **acknowledged**; unacknowledged ones are resent every 500 ms. Sequence starts at 0. |
| 3 | Adapter | `95 19`, total length (= message length + 4), message, checksum. Checksum = two's complement of (length lo + length hi + 88 + sum of message); use 90 instead of 88, and a 16-bit length, when the length is 256 or more. |
| 4 | Session | `1B`, cmd, data. Info requests `01`, mode `11`, challenge `60`, application write `12`, replies `81` / `91` / `E0`. |
| 5 | Charger | The application messages in section 3, carried in a session write: `1B 12 82 <len> <message>`. |

Module layer details:
- Open the data channel: send `3F [03 E8 01]` (channel 1000); the reply is `00 41 <seq> 00 3F`.
- Send data to the charger: `43 [03 E8 | adapter frame]`; the reply is `00 41 <seq> 00 43`.
- Data from the charger arrives as `00 42 <seq> 03 E8 <adapter frame>` and must be acknowledged with `00 02 <seq> 00 42`.

Session start-up (as the vendor app does it): send `1B 11 01 09`, then info requests `1B 01 lo hi` for ids
`0100 0100 0101 0102 0103 0106 0107 2000 2100`, then a challenge `1B 60 <16 random bytes>`. The module answers
with cmd `E0` and 16 bytes, which the app checks. The component implements this check, but the charger works
without it; a failed check is only logged.

## 3. Application messages
All start with `0F` and end with `FF FF`.

**Status query** `0F 03 55 00 55 FF FF`. The reply is 75 bytes: `0F`, length (71), cmd, one unknown byte, then
**four 17-byte slot records**, a checksum (sum of bytes 2..71), `FF FF`. A slot record, big-endian 16-bit fields:

| Offset | Field | Unit / values |
|--------|-------|---------------|
| 0 | mode | 0 Charge, 1 Refresh, 2 Break-In, 3 Discharge, 4 Cycle, 5 Done, 6 Wait, 7 Error |
| 1 | battery present | 0 / 1 |
| 2-3 | capacity | mAh |
| 4-5 | work time | **seconds, restarts at 0 in every phase of a program** |
| 6-7 | voltage | mV |
| 8-9 | current | mA |
| 10 | state | 0 not working, 1 charging, 2 rest, 3 discharging, 4 rest, 5 done, 6 done |
| 11 | temperature | raw - 40 = °C |
| 12 | cycle count | |
| 13-14 | internal resistance | mOhm |
| 15-16 | reserved | |

**Version query** `0F 03 57 00 57 FF FF`. The reply holds the device id as ASCII hex (`NC2500`), the firmware
version and the serial. Other identity strings seen: `AA/AAA CHARGER`, `SkyRC Technology Co.,Ltd`.

**Set slot** (start a mode on one slot), sent as `1B 12 82 11 ...`:

`0F 0D 05 <slot 0-3> <mode> <v1 hi lo> <v2 hi lo> <v3 hi lo> <cycles> 00 00 <chk> FF FF`

with `chk` = sum of bytes 2..13. Mode numbers are as in the table above (0-4). Slot byte 4 means all slots.

| Value | Meaning | Used by |
|-------|---------|---------|
| v1 | charge current, mA | Charge, Refresh, Cycle |
| v2 | discharge current, mA | Refresh, Discharge, Cycle |
| v3 | rated capacity, mAh | Break-In |
| cycles | number of cycles | Cycle |

Limits: charge 200-2500 mA, discharge 100-1000 mA, capacity 500-3500 mAh (all in steps of 100), cycles 1-12.

**Stop.** The vendor app has no stop command, but a set-slot with mode 5 (Done) and all values zero ends a
slot's program: `0F 0D 05 <slot 0-3> 05 00 00 00 00 00 00 00 00 00 <chk> FF FF`. By the next status reply
the slot reports mode 5 and 0 mA, and its work time restarts; the capacity returns to 0 a few seconds later. The
state byte keeps its previous value. A normal set-slot starts the slot again.

System settings (delta-V, temperature cut-off, cut-off voltage, trickle charge) use cmd `0A` (set) and `0B`
(get). They are decoded but not exposed. The app also has firmware-update commands, which this project
deliberately never uses.

## 4. Program sequencing
Observed phases (state byte in brackets):

- **Charge**: charging (1) until the charger's own 300-minute timer, or full.
- **Refresh**: charge (1) 5 h, rest (2) 1 h, discharge (3), rest (2) 1 h, charge (1) 5 h, done (5).
- **Break-In** and **Cycle**: the phase order and durations are are assumed from a published review of the charger and have not been observed.

The work time restarts at 0 at each phase change, and the mode byte stays the same for the whole program.
Progress estimates therefore have to track the phase themselves (see `home-assistant/progress-estimates-package.yaml`).

## 5. Details worth knowing
- The adapter checksum differs for long messages (section 2, layer 3); the status reply is long.
- After an unclean disconnect the charger can take about two minutes to accept a new connection.
- Capacity units were confirmed by arithmetic: 56 mAh after 1019 s at 0.200 A = 0.2 x 1019 / 3600 x 1000.

## 6. Verification status
Checked on a real NC2500:
- Every layer above, and the status query. The four slot records decode correctly.
- Units for voltage, current, capacity, temperature and time.
- **Charge**: set a slot to 0.2 A, 0.3 A and back; the charger followed within one poll.
- **Refresh**: ran a complete program on three NiMH cells, with the phase sequence and the per-phase work time described in section 4.
- **Stop** (set-slot mode 5): stopped a slot during a charge and during a discharge. Mode 5 and 0 mA by the next
  poll, other slots unaffected, and a set-slot afterwards started the slot again.

Decoded from the vendor app but **never tried on a real battery**: the Break-In, Discharge and Cycle set-slot
packets, the system-settings commands, and the exact Break-In and Cycle phase timings.
