# DAC interface revision — 2026-09-14

Engineering draft; the complete controller is **not production-ready**.
This revision follows `b769c0b` and resolves the direct 3.3 V-to-5 V DAC wiring
and the missing firmware-controlled DAC clear connection. It does not provide
a laser safety interlock.

## Circuit and ownership

U_DACLV1 is a TI SN74LVC8T245PWR in the standard KiCad TSSOP-24 footprint.
VCCA and DIR connect to +3V3; VCCB pins 23/24 connect to the **same filtered
+5V_ANA supply as DAC AVDD**, not to a separately powered 5 V rail. OE is low
and pins 11/12/13 return to AGND. R26 remains the existing connection to MCU
ground. Review this return path during layout; do not route SPI over a plane gap.

The native KiCad symbol and manufacturer pin table agree on all 24 pins.
No custom symbol or altered pin type was required. The extra four channels
are unused: A5–A8 share a 10 kohm bias to AGND, B5–B8 are no-connect.
This uses an available standard part with supply isolation and specified delay
across the selected 3.3 V/5 V range.

| Function | ESP32 pin | Buffer input → output | Series resistor | DAC pin | Idle state |
| --- | --- | --- | --- | --- | --- |
| SCLK | GPIO12 / J1_18 | 3 → 21 | R_DACSCLK1 | 7 | LOW |
| DIN | GPIO11 / J1_17 | 4 → 20 | R_DACDIN1 | 8 | LOW |
| SYNC | GPIO10 / J1_16 | 5 → 19 | R_DACSYNC1 | 6 | HIGH |
| CLR | GPIO13 / J1_19 | 6 → 18 | R_DACCLR1 | 5 | HIGH |

Each series resistor is 22 ohm and belongs immediately beside the buffer
output. At its schematic orientation pad 2 faces the buffer, pad 1 faces the
DAC. Both sides have 10 kohm startup bias. Existing R3 remains the DAC-side
CLR pull-up, moved from +3V3 to +5V_ANA and relocated for drawing clarity.
C_DACLVA1 and C_DACLVB1 are 100 nF / 50 V / X7R / 10% local bypass capacitors.

The firmware already uses GPIO13 to assert and release CLR during initialization
(`src/output/galvo_out.cpp`, `galvo::init()` and `dac8562Init()`). Previously that
pin was marked unconnected in the schematic. This revision implements that
existing interface. Firmware, GPIO assignments, SPI mode 1, and the configured
40 MHz clock were not changed.

## Voltage and power-state reasoning

DAC8562 VIH is at least 0.7 times AVDD: 3.5 V at 5 V. Direct ESP32 drive did
not satisfy that requirement. The new B outputs use DAC AVDD as their supply.
The intended regulated rail window for qualification is 4.75–5.25 V and the
MCU rail window is 3.0–3.6 V. The buck and complete power budget must still
prove those limits under load, temperature and transients.

The TI buffer specifies A-input VIH = 2 V for a 3.0–3.6 V input supply and,
at the 4.5 V output-supply test point, VOH >= 3.8 V at -32 mA and VOL <= 0.55 V
at +32 mA. Actual external static loading is approximately 0.53 mA or less
through the 10 kohm bias plus DAC input leakage. The DAC requires VIL <= 0.8 V.
These specifications provide a practical interface margin; final validation
must measure both rails and levels together, including noise and resistor drop.
Operation up to the DAC's absolute supply limit is not assumed from these numbers.

TI specifies Ioff, isolation when either supply is below 100 mV, and supply
disconnect behavior. When +3V3 is absent, the buffer B outputs are high impedance
and DAC-side resistors hold SYNC/CLR high and SCLK/DIN low. When +5V_ANA is absent,
the B supply and DAC AVDD fall together and the translator isolates the powered
MCU from the unpowered DAC side. Verify actual rail discharge and USB-only
operation; the presence of this part does not prove the rest of the board cannot
back-power either rail through other paths.

Reset bias is not a hardware laser-off state. DAC8562 clear is falling-edge
sensitive and clears to zero scale, which is not centered galvos after the
subtractor. A reset/brownout may therefore move an axis. Independent laser
blanking and energy removal remain mandatory unresolved controller design work.

## Timing budget and remaining qualification

For VCCA = 3.3 +/-0.3 V and VCCB = 5 +/-0.5 V over -40 to +85 C, TI specifies
A-to-B propagation delay of 0.5–4.4 ns. Conservatively allowing 3.9 ns difference
between delays, an ideal 40 MHz half-period of 12.5 ns leaves:

| DAC requirement | Minimum | Ideal half-period minus buffer spread | Remaining before source/PCB effects |
| --- | --- | --- | --- |
| DIN setup | 6 ns | 8.6 ns | 2.6 ns |
| DIN hold | 5 ns | 8.6 ns | 3.6 ns |
| SCLK HIGH / LOW | 8 ns | 8.6 ns | 0.6 ns |

These are **budget estimates, not measured compliance**. They assume the
datasheet test loading, clean source edges and ideal source duty cycle. The
buffer's test load at 5 V is 15 pF. DAC input capacitance is specified as 3 pF;
include package/trace capacitance, probe loading, 22 ohm source termination,
ground bounce and the actual ESP32 output timing. The pulse-width budget is
particularly tight. Keep this interface short and qualify at the DAC pins.

DAC SYNC must additionally remain HIGH for >=80 ns, precede the first falling
SCLK by >=13 ns and follow the final falling SCLK by >=10 ns. The firmware uses
manual CS writes and register-update polling; the presence of those operations
alone is not a worst-case delay proof. Capture both the initialization transfers
and the optimized two-channel transfer path, at supported CPU clocks and under
load. CLR LOW must last >=80 ns. If measurements or bounded timing analysis
cannot establish margin, reduce the SPI clock and explicitly enforce CS timing
in a separately verified firmware revision before production release.

## Verification and reproduction

KiCad 10.0.6 ERC: zero violations with all severities enabled and no new
exclusions. The netlist contains 116 components / 104 nets (including unused
buffer outputs and other intentionally unconnected pins). The dedicated check
verifies the exact four signal paths, rails, bias, unused pins and bypass.
With the baseline netlist it also verifies every original component value and
footprint and every unrelated original net membership. It rejects the pre-change
schematic because the translator and its required connections are absent.
Synthetic negative checks also reject a direct MCU-to-5 V bypass and a wrong
DIR strap. ERC and the checker passed again on a relocated archive of the staged
project and tests. Source hash and results are in `2026-09-14-dac-checks.json`.

```sh
kicad-cli sch export netlist --format kicadxml \
  -o /tmp/galvos-dac-netlist.xml \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
python hardware/tests/check_dac_interface.py /tmp/galvos-dac-netlist.xml
kicad-cli sch erc --format json --severity-all --exit-code-violations \
  -o /tmp/galvos-dac-erc.json \
  'hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch'
```

An optional second XML argument to the checker compares the schematic exported
from baseline commit `b769c0b`. It permits only the three original SPI nets to
split across the translator and R3 pin 2 to move to DAC AVDD, plus the specified
new components. GPIO13 was previously an intentionally unconnected singleton.
All other existing connections must remain equivalent regardless of net naming.

The old PCB/Gerbers remain unchanged and invalid as production outputs. No
scope measurements, updated-board DRC, physical bring-up or laser operation
were performed in this revision.

## Primary sources inspected

- [TI SN74LVC8T245, SCES584D](https://www.ti.com/lit/ds/symlink/sn74lvc8t245.pdf),
  pin functions, recommended operating conditions, electrical characteristics,
  section 5.9 delay table, measurement load table and supply isolation behavior.
- [TI DAC8562, SLAS719E](https://www.ti.com/lit/ds/symlink/dac8562.pdf),
  logic levels, input capacitance, clear behavior and section 7.6 timing.
- [Espressif ESP32-S3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf),
  DC characteristics; the actual pin assignments and clock setting were also
  checked against this repository's firmware.
