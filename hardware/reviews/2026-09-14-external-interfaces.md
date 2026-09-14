# v2 external-interface audit — 2026-09-14

**NOT RELEASED FOR MANUFACTURE.** Source commit `40585b3`. This review
compares the current schematic export with actual definitions and uses in
firmware; no schematic, firmware or interface contract was changed.

## Confirmed tachometer assignment mismatch

| Signal | Connector | Actual schematic MCU pin | Required by include/pinmap.h |
| --- | --- | --- | --- |
| FAN1_TACH | J5.3, R30 pull-up | J1_15 = GPIO9 | PIN_FAN1_TACH = GPIO2 |
| FAN2_TACH | J7.3, R31 pull-up | J2_5 = GPIO2 | PIN_FAN2_TACH = GPIO9 |

The GPIO identities above come from exported U1 pin functions, not a guess
based on connector positions. `docs/03-build-and-config.md` and the
repository instructions also specify fan 1 on GPIO2 and fan 2 on GPIO9.
Both pull-ups are 4.7 kohm to +3V3. The PWM signals are correctly assigned:
J5.4 to GPIO16 and J7.4 to GPIO17.

The mismatch has no demonstrated current RPM-readout symptom: searches
find only the tach pin definitions and documentation, and the temperature
module does not implement tach acquisition or stalled-fan detection.
It will misidentify the fan if later firmware uses the documented pin map
with this v2 schematic. ERC cannot detect this contract error.

Proposed bounded fix: swap only the MCU destinations of FAN1_TACH and
FAN2_TACH in the schematic. Keep J5/J7 pin assignments, R30/R31, PWM wiring,
firmware definitions, values and footprints unchanged. Prove the exact two
intended net-membership changes against a baseline and rerun the GPIO
comparison, DAC checks and ERC. This fix is **pending approval**, not done.

## Other GPIO assignments checked

A one-off comparison read numeric `PIN_*` definitions from `include/pinmap.h`
and traced each endpoint's net to the exported U1 GPIO function. Of 17
comparisons, 15 match and the two tach signals above do not:

- SD connector J1: CS 42, MOSI 6, SCK 5, MISO 1.
- Fan PWM: J5.4 = GPIO16 and J7.4 = GPIO17.
- DMX RO: J_DMX1.3 = GPIO4.
- E-stop: J_ESTOP1.1 = GPIO47.
- Shared 1-Wire: U6.2 = GPIO18; U7..U10 share the same bus as already
  established in the package audit.
- Scan status: U_SCAN1.3 = GPIO39.
- Laser-power enable: U_WD1.4 (RESET) = GPIO38.
- Watchdog heartbeat: C_TRIGC_WD1.1 = GPIO14.
- RGB resistor inputs: R1.1 = GPIO7, R2.1 = GPIO8, R4.1 = GPIO21.

The DAC's four translated signals are covered separately by the existing
DAC interface checker. `src/storage/sd_card.cpp` uses SPIClass(HSPI), mapped
to SPI3 on this target; `src/output/galvo_out.cpp` uses SPI2_HOST for the DAC.
The schematic preserves the separate GPIO sets. Do not merge these buses.

## Electrical and external-contract gates

| Interface | Established topology | Still required |
| --- | --- | --- |
| SD J1 | Pin 1 +3V3; 2 CS; 3 MOSI; 4 SCK; 5 MISO; 6 GND | Exact module; whether its regulator/level shifters work from 3.3 V; card current/startup; signal integrity and cable length |
| DMX module J_DMX1 | Pin 1 buck +5V; 2 GND; 3 RO directly to GPIO4; 4 NC | Module schematic and guaranteed RO levels; receiver enable/bias/termination; actual bus connector and cable protection |
| Fans J5/J7 | 1 GND; 2 buck +5V; 3 tach pulled to +3V3; 4 direct GPIO PWM | Fan voltage/current; tach electrical type; PWM pull-up voltage, input type and polarity; connector contract |
| Sensors U6..U10 | 1 GND; 2 shared 1-Wire; 3 +3V3 | Probe wiring; cable capacitance/length/topology; bus timing, ESD and cable-short behavior |
| RGB U4/U5/U3 | Pin 1 GND; pin 2 respective 6N137 output and 1k/1k divider from +3V3 | Driver's guaranteed TTL thresholds, input current and fault states; independent power/gating safety |

The scan timer output is directly connected to GPIO39 despite the timer
being supplied from 5 V. The 5 V DMX module's RO has no level translation
in the board netlist either. Neither connection has a demonstrated
ESP32-safe voltage envelope. A matching GPIO number does not validate
voltage levels, power-off injection or an external module's circuitry.

Fan software initializes two 25 kHz LEDC outputs and sets duty 255 at
startup (`src/sensors/temp_monitor.cpp`). It does not configure an explicit
open-drain PWM interface there. Existing comments refer to HW-517 MOSFET
modules, while the v2 drawing has direct four-pin fan connections. Resolve
which hardware is intended before choosing a driver or assuming that duty
255 produces the intended cooling state.

## Safety boundary clarification

The E-stop connector reaches a GPIO only. `safety::task()` interprets HIGH
as OK, so an open connector/wire is accepted by that software input.
`safety::allOk()` returns `s_user_arm_request` whenever `safety_override`
is enabled. It therefore bypasses E-stop, scan, software watchdog and
subsystem checks, **but retains the user arm request**. Do not incorrectly
describe this particular branch as bypassing the arm-request condition.
The result controls GPIO38 and hence the watchdog timer's RESET input.

No direct E-stop-to-power-gate path or independent mirror-feedback safety
has been established. Firmware override changes and a new safety circuit
must not be quietly bundled into the tach/BOM corrections. The Class 4
laser needs a reviewed independent shutdown architecture and physical
fault tests; pin-map corrections do not satisfy that gate.

## Next implementation checkpoint

There are now two concrete, bounded schematic corrections to review:
the five sensor-interface symbol/BOM identities from the package audit,
and the two swapped tach destinations identified here. Neither requires
inventing fan electrical characteristics or changing firmware behavior.
Both remain pending the explicit design-approval checkpoint. Wider power,
connector and safety design still needs the requested external specifications.
