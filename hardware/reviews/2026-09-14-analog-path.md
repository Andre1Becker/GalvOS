# v2 analog-path audit — 2026-09-14

Status: **NOT RELEASED FOR MANUFACTURE.** Analysis only; no schematic,
firmware, safety gate or calibration setting changed in this review.
The existing PCB is still not a v2 implementation.

## Evidence and scope

Source: `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch`
at commit `68b3d4a`; exact source hash and calculated results are in
`2026-09-14-analog-checks.json` alongside this document.
Fresh KiCad XML export: 116 components / 104 nets. ERC: zero violations.
The existing DAC interface checker also passes against the pre-translator
baseline. These checks do not establish analog performance or laser safety.

The calculation checked 17 exact analog net memberships, 21 relevant values
and ground connections against the export, not just schematic labels.
X and Y have equal nominal component values:

| Function | X | Y | Value |
| --- | --- | --- | --- |
| DAC source isolation | R11 | R13 | 100 ohm |
| Local shunt capacitor | C3 | C4 | 1 nF |
| Sense branch link | X_NODE1 | Y_NODE1 | 0 ohm |
| Inverting input resistor | R7 | R14 | 10 kohm |
| Feedback resistor | R12 | R17 | 22 kohm |
| Reference divider, upper | R10 | R15 | 10 kohm |
| Reference divider, lower | R9 | R25 | 22 kohm |
| Connector source isolation | R6 | R16 | 100 ohm |
| Common trigger coupling | C_SCANX1 | C_SCANY1 | 100 nF |

U12C buffers U2's reference. U13 pins 2/3 carry X/Y and pin 1 is AGND.
Both sense capacitors meet at SCAN_TRIG, with a 10 kohm pull-up to +5 V,
NE555 pin 2 and a diode whose cathode is SCAN_TRIG and anode is GND.
R26 joins GND and AGND through a nominal 0 ohm resistor.

## DC transfer and range

Assumptions: ideal op-amps and reference buffer, Vref = 2.5 V, DAC gain 2,
nominal resistor values, open-circuit connector, negligible DC capacitor
leakage. DAC internal gain 2 agrees with the reference-enable command in
`src/output/galvo_out.cpp`, function `dac8562Init()`.

Let Rs = 100, Ri = 10000, Rf = 22000, Rt = 10000 and Rb = 22000 ohm.
The source resistor participates in the DC gain because Ri draws current:

```text
Vp = Vref * Rb / (Rt + Rb) = 1.71875 V
g = Rf / (Ri + Rs) = 2.178217822
Vdac(D) = 2 * Vref * D / 65536
Vop(D) = (1 + g) * Vp - g * Vdac(D)
       = 5.462561881 - 2.178217822 * Vdac(D)
```

| DAC code | DAC voltage (V) | Op-amp output (V) |
| --- | ---: | ---: |
| 0x0000 | 0 | +5.462562 |
| 0x0666, default lower clamp | 0.124969 | +5.190351 |
| 0x8000, center | 2.500000 | +0.017017 |
| 0xF999, default upper clamp | 4.874954 | -5.156150 |
| 0xFFFF | 4.999924 | -5.428361 |

The nominal mathematical zero is code 32870.4, not 32768. The nominal
open-circuit codes inside +/-5 V are 2784 through 62957 inclusive. These are
**not recommended release limits**: there is no tolerance or load margin.
The current clamp spans about 95% of the code range, not 95% of the stated
galvo +/-5 V input range. Do not confuse input rating with OPA output clipping.

The configurable `outputScale = 0.91` in `include/config.h` can reduce a
nominal full-range waveform to approximately +4.972 / -4.938 V where that
scale applies. It is not an immutable voltage limit, does not correct the
zero offset and does not make the wider clamp a +/-5 V guarantee.
Firmware comments describing a 2x difference amplifier or a 0..Vref DAC
output are not the transfer function of this v2 circuit.

For a purely resistive load RL from a connector signal to AGND:
`Vconnector = Vop * RL / (RL + 100 ohm)`. At RL = 10 kohm the factor is
0.990099. Actual driver impedance, differential/single-ended input contract,
cable capacitance and ground offset must be established; this example is not
a measurement or an assumed galvo specification.

The DAC's nominal DC source/sink current ranges from -170.17 to +324.88 uA
over 0..5 V, using `(Vdac - Vp) / 10100`. The reference buffer supplies
156.25 uA to the two dividers; U2's reference mainly sees U12C's input.

### Tolerances are not specified by the present BOM

The relevant resistor fields do not specify tolerance or selected MPNs.
As an illustration only, independent +/-1% corners on all five resistors
produce the following ranges, with all active devices and Vref still ideal:

| DAC code | Minimum output (V) | Maximum output (V) |
| --- | ---: | ---: |
| 0x0666 | +5.087802 | +5.294969 |
| 0x8000 | -0.052105 | +0.084267 |
| 0xF999 | -5.329781 | -4.986462 |

These are resistor-only corner calculations, not total guaranteed limits.
DAC gain/offset/nonlinearity, reference error, op-amp offsets, temperature,
load and supply behavior remain to be budgeted. Shared reference error
scales both terms; it must not be treated as two independent references.

## Scan coupling changes the signal path

The sense branches are connected to the precision DAC nodes, not isolated
copies. Because both capacitors share a trigger node, they also form an
AC path between axes. A stationary DAC code does not imply a stationary
analog output when the other axis moves.

For a reproducible *unclamped linear RC model*, define small-signal nodes
`v = [x, y, trigger]`, ground both supplies in AC, hold the op-amp input
voltages constant, and treat both DAC outputs as ideal voltage sources.
Ignore diode leakage, NE555 input/clamp behavior, DAC output impedance,
op-amp bandwidth and PCB/cable parasitics. R26 is an ideal short here.

```text
G = diag(0.0101, 0.0101, 0.0001) siemens
C = [[101,    0, -100],
     [  0,  101, -100],
     [-100,-100,  200]] * 1e-9 farad
(G + j*2*pi*f*C) * v = [Vdac_x/100, Vdac_y/100, 0]
Vop_x_ac = -2.2*x; Vop_y_ac = -2.2*y
```

For 1 V AC on DAC X and zero AC on DAC Y, calculated magnitudes are:

| Frequency | X node (V/V) | Uncommanded Y node (V/V) | Trigger (V/V) |
| --- | ---: | ---: | ---: |
| 100 Hz | 0.988590 | 0.002402 | 0.386194 |
| 1 kHz | 0.986231 | 0.030489 | 0.491073 |
| 10 kHz | 0.877156 | 0.259470 | 0.492586 |
| 25 kHz | 0.676267 | 0.411383 | 0.492549 |
| 50 kHz | 0.560507 | 0.464533 | 0.492374 |
| 100 kHz | 0.514262 | 0.480751 | 0.491669 |

At 10 kHz, for example, the ideal op-amp model predicts an unintended Y
output magnitude of 0.570834 V per 1 V DAC-X excitation. These figures are
conditional network calculations, **not simulated or measured complete
device behavior**. In particular, trigger overvoltage can activate unknown
internal current paths and invalidate the linear approximation.

For equal-and-opposite DAC changes, trigger contributions cancel exactly
with nominal matching. Each axis then has effective capacitance 101 nF,
time constant `(100 || 10000) * 101 nF = 10 us` and pole 15.915 kHz.
With the scan branches absent, the local 1 nF RC pole would be 1.607 MHz;
that is not the bandwidth of the complete DAC/op-amp system. The RC model's
three time constants are 0.098522 us, 10 us and 2009.901478 us. DC,
differential closed form, axis symmetry, trigger cancellation and stable
poles were checked numerically.

The DAC datasheet characterizes capacitive stability at 1 nF with no load
and 3 nF with 2 kohm load, and gives a typical slew rate of 0.75 V/us.
Here the large capacitors are behind 100 ohm resistors: comparison of raw
capacitance alone cannot establish instability. Nevertheless, tracking a
0.75 V/us differential ramp at the sense nodes would require about
75.75 mA per 101 nF, before resistive current. The nodes cannot be assumed
to follow an unloaded DAC waveform. Loaded settling and stability need
proper device modeling and bench measurement.

### Trigger sensitivity and overvoltage

For an ideal instantaneous -5 V step on X alone, the unclamped model gives
a minimum trigger voltage of 2.538 V from an initial 5 V, around 0.978 us.
This is above even the NE555's 2.2 V maximum trigger threshold at VCC = 5 V
(typical 1.67 V, minimum 1.1 V). Real DAC slew makes the ideal step an
illustration, not a timing qualification. Equal-and-opposite X/Y motion
can cancel completely; this circuit cannot reliably detect arbitrary
command motion, let alone actual mirror motion.

A +5 V single-axis step instead predicts a trigger peak of 7.462 V. The
fitted diode only clamps negative voltage; it provides no upper clamp.
NE555 trigger absolute input rating is at most VCC. Real overvoltage and
injection current depend on unmodeled input structures and DAC slew, but
there is **no demonstrated upper-voltage protection** in the netlist.
The same-polarity dual-axis ideal step would double the excursion.

## Required design decisions and next work

1. Do not remove the scan branches merely to improve waveform quality:
   their apparent protection function needs a reviewed replacement and
   fault behavior. Establish independent laser shutdown ownership and actual
   mirror-feedback availability first. The command detector is not a
   substitute for an independent scan-safety system.
2. A replacement command-monitoring path should sense each axis at high
   impedance, prevent analog cross-coupling, enforce input voltage limits
   and combine detection after per-axis conditioning. Threshold, timeout,
   restart and single-fault behavior require an explicit design. A clamp
   alone would not solve cross-coupling or motion cancellation.
3. Set the required connector transfer and tolerance budget before changing
   resistor ratios or firmware calibration. Account for source isolation in
   both sides of the difference-amplifier ratio. Reducing firmware range
   alone does not repair scan loading, offset or power-sequence behavior.
4. On a reviewed prototype with laser power physically inhibited, measure
   DC endpoints/center, X-to-Y feedthrough, loaded settling, trigger voltage,
   and equal/opposite/single-axis trajectories. Sweep real cable/driver
   loads. Check trigger and op-amp supply faults, reset and startup. With a
   live 2.5 V reference and zero DAC code, the nominal output is +5.463 V,
   not center; software limits do not apply before initialization.

No protection bypass, new component selection or calibration was adopted.
Supply-fault behavior, complete error/bandwidth budgets and physical
qualification remain open acceptance items in `codex-todos.md`.

## Manufacturer sources and reproduction

- [TI DAC8562, SLAS719E](https://www.ti.com/lit/ds/symlink/dac8562.pdf):
  electrical characteristics; sections 8.3.1.1 and 8.3.1.2; reference control.
- [TI OPA4134](https://www.ti.com/lit/ds/symlink/opa4134.pdf): input offset,
  bandwidth, slew rate and output-load specifications (not included in the
  ideal RC model).
- [TI NE555, SLFS022K](https://www.ti.com/lit/ds/symlink/ne555.pdf): absolute
  maximum input voltage and 5 V trigger electrical characteristics.

```sh
kicad-cli sch export netlist --format kicadxml \
  -o /tmp/galvos-analog-netlist.xml \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o /tmp/galvos-analog-erc.json \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python hardware/tests/check_dac_interface.py /tmp/galvos-analog-netlist.xml
```

The equations and matrices above define the numerical calculation without
a proprietary simulator. Values in the companion JSON are analytical-model
results, not a production acceptance test or a persistent regression runner.
