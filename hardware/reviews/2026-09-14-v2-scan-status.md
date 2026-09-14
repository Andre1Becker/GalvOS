# V2.0.4 scan-status input protection

Engineering change, not approval for fabrication or laser operation.

## Circuit change

V2.0.3 connected the 5 V NE555 output U_SCAN1.3 directly to U1.J2_9
(ESP32 GPIO39). V2.0.4 inserts a noninverting, 5.5 V-tolerant
SN74LVC1G17DBVR Schmitt buffer, supplied by the MCU's own +3V3 net.
The original timer output becomes SCAN_STATUS_5V; GPIO39 is now driven
only by the buffer output.

| Item | Connection / requirement |
|---|---|
| U_SCANLV1.2 (A) | U_SCAN1.3 and R_SCANIN1.1 only |
| U_SCANLV1.4 (Y) | U1.J2_9 (GPIO39) only |
| U_SCANLV1.5 (VCC) | MCU +3V3, not the timer/buck 5 V rail |
| U_SCANLV1.3 (GND) | MCU/timer power ground, not the analog side of R26 |
| U_SCANLV1.1 (NC) | Unconnected |
| R_SCANIN1 | 10 kohm, 1%, 0.25 W, 1206; input to power ground |
| C_SCANLV1 | 100 nF, X7R, 50 V, 10%, 1206; local buffer supply bypass |

TI DBV is the five-pin SOT-23 package; the standard SOT-23-5 footprint
retains its 0.95 mm lead pitch and the manufacturer's 1=NC, 2=A, 3=GND,
4=Y, 5=VCC mapping. The exact orderable buffer MPN and manufacturer are
stored in both schematic and PCB fields. Passive requirements are specified,
but their purchase MPNs still require BOM qualification.

Firmware is unchanged: HIGH still means scan status OK. The pull-down biases
the buffer input LOW if the timer output is disconnected while the buffer is
powered normally. Do not infer a fault-safe output for an absent/failed buffer:
the firmware still enables GPIO39's internal pull-up and initially sets
scanfail_ok=true. This stage is input protection, not a safety interlock.

## Manufacturer evidence and its limits

Sources downloaded on 2026-09-14:

- [TI SN74LVC1G17, SCES351Y](https://www.ti.com/lit/ds/symlink/sn74lvc1g17.pdf),
  revised October 2025, sections 4, 5.3, 5.5, 7 and 8.
  SHA-256: c76fa723fac4502423967a1aa087dd669106b52b8de620c024b26e56f5d59309.
- [TI NE555, SLFS022K](https://www.ti.com/lit/ds/symlink/ne555.pdf),
  revised March 2026, section 5.5.
  SHA-256: c6f5275b8c31eb1d65e30400e38357aefe95e2088286bbded5afc216f4259ee9.

The buffer permits 0..5.5 V input in recommended operation and specifies
partial-power-down support (Ioff, up to 10 uA at VCC=0). Its input must still
remain within that range, including ripple, overshoot and fault transients.
This is not a 12.6 V-tolerant interface.

At the documented 3 V test point, TI specifies Schmitt thresholds
VT+ = 1.48..1.92 V and VT- = 0.89..1.20 V. Input leakage is at most
5 uA; with a 10 kohm +1% pull-down, leakage alone gives at most 50.5 mV.
At 5.5 V the resistor's worst-case load is 5.5/9900 = 0.556 mA and its
dissipation is approximately 3.06 mW.

The NE555 table specifies VOH >= 2.75 V at VCC=5 V, IOH=-100 mA,
and VOL <= 0.35 V at VCC=5 V, IOL=5 mA. These entries are at 25 C.
They are useful reference points, not a full temperature/supply/load
qualification. In particular, the buffer's 3 V threshold row must not be
silently treated as a guaranteed threshold range at every 3.3 V rail corner.
Confirm logic margins against the actual 3V3/5V supplies, chosen timer and
operating temperature; measure both edges and power sequencing.

The buffer's noninverting truth table preserves polarity. Its output is
referenced to MCU VCC, avoiding the original direct 5 V output connection.
For light loads, TI specifies VOH >= VCC-0.1 V and VOL <= 0.1 V at 100 uA.
The GPIO configuration and real load remain part of qualification.

## Verification

The exported baseline must fail the new scan-status checker. The revised
export must pass both its explicit connectivity/BOM checks and preservation
comparison: exactly three components are added; only the original status net,
3V3 and power-ground memberships change. All other component values,
footprints and electrical memberships must remain identical.

Eight unit tests exercise the contract, including direct 5 V bypass, wrong
supply, wrong bias rail, wrong bypass return, missing bias, an inverting part
and an incorrectly connected NC pin. These are structural tests, not an
electrical simulation or fault-safety certification.

Run the existing fan-power, sensor/tach and DAC interface tests as well as
fresh native ERC, DRC with zone refill and schematic parity, and the PCB
draft-evidence guard. Final measured counts and hashes are maintained in
[CURRENT-HARDWARE.md](CURRENT-HARDWARE.md).

## Confirmed socket plan

The user's two 22-position female socket strips are already represented by
U1's Assembly field and its 44 plated pads (J1_1..22 and J2_1..22). No header
pin, hole or route change is needed to select socketed assembly. Corrected
the stale footprint description that incorrectly said 21/20 pins.

The retained pitch is 2.54 mm and provisional row spacing is 22.86 mm.
Confirm row spacing, orientation, socket body/pin/hole fit and mating height
with the actual purchased board and socket strips. N16R8 alone does not
confirm USB or antenna geometry. No antenna/USB clearance approval is implied.

## Remaining gates

This change does not qualify the scan-trigger circuit, detect actual mirror
motion, add independent shutdown/rearm, fix the DMX 5 V input, or change
E-stop/override/RGB behavior. All prior power, return-path, timing, thermal,
mechanical, EMC and physical safety requirements remain open.
