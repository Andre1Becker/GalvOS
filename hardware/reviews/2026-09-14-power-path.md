# v2 buck and power-path audit — 2026-09-14

**NOT RELEASED FOR MANUFACTURE.** This review records calculations and
verified connectivity, not a qualified power supply. No schematic or
firmware changes were made. Source schematic at commit `9aa6ad5` has SHA-256
`1a01160112f6f8824b9dd158ffd85847ce2014c21dd0d4af20725a67be076cbc`.

## Verified power paths

```text
J2.1 (VIN_RAW) -> D2 anode -> D2 cathode -> VIN_BUCK -> U_BUCK1
U_BUCK1.SW -> L1 -> Buck +5V
  -> J4.1 / J5.2 (fan 1 power)
  -> J6.1 / J7.2 (fan 2 power)
  -> D1 anode -> D1 cathode -> DevKit J1_21 (+5V_MCU)
  -> FB1 -> +5V_ANA (DAC and translator B supply)
  -> optocoupler output supplies, NE555 timers and DMX module supply
```

J4/J6 are labeled `FAN1 PWR IN` / `FAN2 PWR IN`, but are electrically
on the regulator's output, without diode isolation or a load switch.
An external supply connected there is directly paralleled with the buck
output. Do not connect an independent fan supply to these pins on the
current design. Fan voltage/current and the intended connector contract
must be confirmed before retaining this topology or changing the labels.

D2 is a series 1N5822 at the raw input. It does not isolate an external
supply applied at the buck output from the regulator itself. TI section 10
warns that an output above VIN can discharge through the SW-to-VIN parasitic
diode, with possible damage from uncontrolled current. No coordinated
input-short/backfeed protection is established here.

D1 is a 1N5819, oriented to supply the DevKit from the buck. Its orientation
blocks the simple reverse path through D1, but is not proof of USB-host or
whole-board power isolation. The exact DevKit power schematic, D1 drop and
current/thermal ratings, USB-only state and alternate signal paths remain
to be checked. +5V_MCU is diode-reduced, not a regulated 5 V rail.

The one-off XML check verified eight exact nets, eleven output-rail pin
memberships, twelve ground memberships and thirteen relevant values.
Fresh all-severity KiCad ERC reports zero violations; the DAC interface
checker passes including preservation against the pre-translator baseline.
Saved calculation values were independently recomputed from their formulas.
These checks do not establish power integrity or physical safety.

## Present buck components

| Function | Present value | Qualification gap |
| --- | --- | --- |
| U_BUCK1 | LMR33630ADDA, 400 kHz nominal | Actual current and thermal budget |
| L1 | 6.8 uH; MWSA1204S-6R8 footprint | No selected MPN, tolerance, DCR, hot saturation or RMS rating |
| C_IN1/2 | 2 x 10 uF, 1210 | Effective capacitance, voltage, ripple rating and MPN absent |
| C_INHF1 | 100 nF, 1206 | TI specifies a local 220 nF input bypass for this DDA example |
| C_OUT1/2 | 2 x 22 uF, 1206 | Effective capacitance, ESR, voltage and MPN absent |
| C_BOOT1 | 100 nF | Nominal matches TI; ceramic type and at least 10 V rating unspecified |
| C_VCC1 | 1 uF | Nominal matches TI; ceramic type and 16 V rating unspecified |
| R_FB1/2 | 100 kohm / 24.9 kohm, 1% | Output-error budget |
| R_EN1/2 | 63.4 kohm / 10 kohm, 1% | Startup margin after D2 and supply droop |

The inductor footprint name is not a procurement specification or a proof
of saturation current. Likewise, capacitor package size does not establish
capacitance under DC bias. TI's 400 kHz / 5 V example uses 8 uH and four
22 uF, 16 V, 1210 output capacitors. A difference from that example is not
alone evidence of failure; the actual design needs its own bounds and tests.

## DC regulation and enable thresholds

With ideal feedback input current, `Vout = VFB * (1 + R_FB1/R_FB2)`.
Nominal output is 5.016064 V. Combining the specified 0.985..1.015 V FB
reference with independent 1% resistor corners gives 4.862490..5.173655 V.
This excludes line/load transients, PFM effects and input-current error.
The specified maximum 50 nA current into FB contributes approximately
`IFB * R_FB1`, up to 5.05 mV with the upper resistor at +1%.

The enable divider ratio is 7.34. With TI's rising EN threshold
1.2 / 1.231 / 1.26 V (minimum / typical / maximum):

- Typical start: 9.035540 V at VIN_BUCK, **after** D2.
- Rising-threshold/resistor corners: 8.657347..9.409782 V after D2.
- Typical stop using 100 mV typical hysteresis: 8.301540 V after D2.
  This is not a guaranteed falling-threshold interval.

At the schematic-labeled 10 V raw lower limit, the worst calculated rising
corner leaves only 0.590218 V for D2 drop, cable drop and supply sag together.
Consequently the `10-30V` label is not a demonstrated startup specification.
Startup input-capacitor charging, current-limited sources, temperature and
real D2 characteristics need to be included.

## Inductor ripple and current-limit headroom

The following continuous-conduction estimates use **5.000 V** output,
400 kHz, 6.8 uH and ideal switches. Input values refer to the buck pin,
not the raw connector. Actual input is reduced by D2 and wiring.

```text
D = Vout / Vin
delta_IL = Vout * (1 - D) / (f * L)
Ipeak = Iout + delta_IL/2
Ivalley = Iout - delta_IL/2
IL_rms = sqrt(Iout^2 + delta_IL^2/12)
```

| VIN_BUCK | Ripple, peak-to-peak | Peak at 3 A load | Ripple / 3 A |
| ---: | ---: | ---: | ---: |
| 10 V | 0.919118 A | 3.459559 A | 30.64% |
| 12 V | 1.072304 A | 3.536152 A | 35.74% |
| 24 V | 1.455270 A | 3.727635 A | 48.51% |
| 30 V | 1.531863 A | 3.765931 A | 51.06% |

TI recommends normally choosing 20..40% ripple, using the device's 3 A
rating even for a lighter application load. Its equation 5 gives a nominal
minimum inductance of `0.28 * 5 / 400000 = 3.5 uH`; passing that rule alone
does not prove acceptable ripple or output capability.

The A-version switching-frequency specification is 340..460 kHz. As an
**illustration, not an assigned L1 tolerance**, at L = 6.8 uH minus 20%,
340 kHz and VIN = 30 V, ripple becomes 2.252739 A and the 3 A-load peak
becomes 4.126370 A. The high-side current-limit minimum is only 3.85 A
(typical 4.5 A, maximum 5.05 A). Thus full 3 A operation over these
conditions is not guaranteed by the present nominal values.

Nominal inductor RMS current at 30 V / 3 A is 3.032417 A. TI recommends
saturation capability at least as large as the high-side limit; account
for its 5.05 A maximum, hot inductance and the manufacturer's saturation
definition. Its minimum fallback criterion also involves the low-side
limit, specified as 2.9 / 3.5 / 4.1 A. There is no selected L1 datasheet to
qualify either criterion, short-circuit behavior or winding temperature.

## Capacitors, transients and thermal limits

For an **illustrative** 2 A load step and 250 mV allowed output deviation,
TI equation 6 with the nominal ripple factors above gives lower-bound
effective capacitance of 43.406, 45.250, 49.922 and 50.867 uF at 10, 12,
24 and 30 V respectively. These are design estimates, not measured response.

```text
K = delta_IL / 3 A
Cmin = delta_I / (f * delta_V * K)
       * ((1-D)*(1+K) + K^2/12*(2-D))
```

C_OUT1/2 provide only 44 uF nominal locally, before tolerance, temperature
and DC-bias loss. For illustration, -20% tolerance and a further 10% bias
loss reduce that to 31.68 uF; the actual 1206 parts have no specified loss
curve. The three optocoupler bypasses and two timer bypasses add 0.5 uF
nominal on the same rail but are distributed. Capacitance behind FB1 or D1
must be evaluated with the intervening impedance, not simply counted as
local output capacitance. A smaller actual load step may need less;
therefore the assumed 2 A step must not become an invented product requirement.

Ignoring ESR and derating, 44 uF gives about 10.880 mV peak-to-peak capacitive
switching ripple at 30 V using `delta_IL/(8*f*C)`. Small steady-state ripple
does not imply adequate load-step response or loop stability. TI explicitly
requires load-transient and Bode-plot validation before production.

The input bank needs at least 10 uF effective ceramic capacitance per TI,
with voltage rating at least the maximum application input, preferably
twice that value. The present nominal 20 uF cannot establish that minimum
without bias curves. Input capacitor worst-case RMS switching current is
approximately Iout/2: 1.5 A at a 3 A load. Layout must place the local input,
BOOT and VCC bypasses at their IC pins with short return loops.

There is no demonstrated input fuse/TVS/lead-damping design. TI warns that
long supply leads and low-ESR ceramics can ring above the source voltage,
and discusses damped bulk capacitance and input-short/output-backfeed
protection. Choose these only after source impedance, wire length, maximum
voltage/transients and fault energy are known. Do not add an arbitrary TVS:
its clamp voltage, source fault current and fuse coordination matter.

The regulator's 36 V operating limit does not make a 30 V source safe
against unspecified transients. IC, capacitors, D2, connectors and traces
all require their own voltage/current/thermal checks. The datasheet's
JEDEC thermal resistance is explicitly not a board-design guarantee.
Without load profile, ambient limit, enclosure airflow, copper and thermal
via geometry, no continuous output-current rating can be assigned here.

## Next decisions and proof

1. Confirm actual input range and supply fault/current behavior; list every
   5 V load, both fan nameplates, startup/stall currents and possible external
   supplies. Resolve J4/J6 ownership and the exact DevKit USB power circuit.
2. Select L1 and capacitor MPNs with derating curves, then choose L/C values
   against worst-case ripple/current limits, required load step and startup.
   Correct the local DDA input bypass to TI's 220 nF recommendation as part
   of that reviewed BOM/layout change. Do not blindly copy a reference design.
3. Specify input protection and reverse-energy handling. Lay out a matched
   PCB with current loops, grounds, thermal pad/vias and feedback routing
   reviewed before assigning a supply rating.
4. With laser power physically inhibited, qualify startup at both input
   limits, input hot-plug/ringing, full load, fan starts/stalls, load steps,
   loop stability, output ripple and temperatures. Use controlled,
   current-limited setups for input-short/backfeed and mixed-power tests.

Reference: [TI LMR33630, SNVSAN3F](https://www.ti.com/lit/ds/symlink/lmr33630.pdf),
sections 7.3–7.5, 9.2.2.4–9.2.2.8 and 10. Equations 5–7 were also inspected
in the rendered PDF to avoid text-extraction ambiguities. Inspected PDF
SHA-256: `3b0920a4e56a0b3f5f214a6317d3c06cff6e541e5054024adc31c30acc50c04a`.

Reproduce connectivity export/ERC using the commands in the analog review,
substituting `/tmp/galvos-power-netlist.xml` and `/tmp/galvos-power-erc.json`.
The companion `2026-09-14-power-checks.json` records calculation results and
checks; it is not a persistent regression runner or a physical test report.
