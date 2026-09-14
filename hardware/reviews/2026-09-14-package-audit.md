# v2 package and assembly audit — 2026-09-14

Follow-up: the U6..U10 connector-identity correction below was approved and
implemented in [hw-v2.0.1-draft](CURRENT-HARDWARE.md). Findings below describe
the pre-correction source; other qualification gaps remain open.

**NOT RELEASED FOR MANUFACTURE.** No schematic or footprint edits in this
review. Source commit: `03071ec`; schematic SHA-256:
`1a01160112f6f8824b9dd158ffd85847ce2014c21dd0d4af20725a67be076cbc`.

## Verified structural coverage

A fresh KiCad XML export contains 116 components using 21 distinct assigned
footprints. All 21 resolve through the project-local or installed KiCad
libraries. For every component, the set of numbered footprint pads equals
the set of exported symbol pins, including no-connect nets. No missing or
extra numbered pads were found. Repeated ground-pad numbers and unnumbered
paste/mask apertures were not treated as extra electrical terminals.

This is a structural check, **not** a full pin-function, dimensional,
manufacturer-part or assembly-process qualification. A wrong pin name can
still have a matching pad number. Installed standard library geometry also
needs to be frozen in the eventual released PCB/library set.

## Custom IC land patterns

| Component | Assigned land pattern | Measured pad geometry | TI example |
| --- | --- | --- | --- |
| U2 DAC8562SDGST | Footprint:DGS10 | 0.5 mm pitch; pad 1 center (-2.0955, -1.000001) mm; pad 1.5748 x 0.2794 mm | DGS0010A: 0.5 mm pitch; row separation 4.4 mm; pad 1.45 x 0.30 mm |
| U12 OPA4134UA/2K5 | Footprint:D14 | 1.27 mm pitch; pad 1 center (-2.4638, -3.81) mm; pad 1.9812 x 0.5588 mm | D0014A: 1.27 mm pitch; row separation 5.4 mm; pad 1.55 x 0.60 mm |

Both have the expected counterclockwise top-view numbering: U2 pins 1..5
down the left row and 6..10 up the right; U12 pins 1..7 down the left and
8..14 up the right. Both custom footprints have courtyard, fabrication and
silkscreen geometry. Neither embeds a 3D model.

The land dimensions differ from TI's examples, but difference alone is not
a defect. The installed generic KiCad alternatives also use different land
dimensions: MSOP-10 uses 1.50 x 0.35 mm pads at x = +/-2.1 mm; SOIC-14 uses
1.95 x 0.60 mm pads at x = +/-2.475 mm. Replacing custom footprints merely
to match a generic library would not itself establish TI-example compliance
or better soldering. Select the assembly process and inspect heel/toe/side
allowances, mask clearance and stencil apertures before changing geometry.

Sources: TI DAC8562 SLAS719E, package drawing DGS0010A and board-layout
example; TI OPA4134 datasheet, D0014A package and board-layout example.
These are the manufacturer documents used in the analog review.

## Buck exposed pad and MCU module

U_BUCK1 uses the native Texas HSOP-8 thermal-via footprint whose description
references DDA0008J. Its 2.95 x 4.9 mm front/back copper regions numbered 9
are **not** the exposed package-pad dimensions: the footprint uses separate
mask/paste features, consistent with TI's extended, mask-defined copper
concept. Do not reject or replace it solely by comparing that copper area
with the package's smaller exposed pad. The future layout must qualify
the exact purchased package variant, stencil, via filling/tenting and
thermal path. Multiple pads/vias numbered 9 correctly represent one ground
terminal; current netlist connects it to power ground.

U1's custom module footprint has 44 pads, row spacing 22.86 mm, 2.54 mm
pitch, pad diameter/width 1.53 mm and drill 1.02 mm. Pad names J1_1..J1_22
and J2_1..J2_22 match the symbol. This does not identify the actual DevKit:
header locations, USB overhang/access, installed header type, underside
clearance and antenna keepout must be measured or checked against the
specific supplier drawing. Do not infer these from the ESP32 module name.

## Confirmed assembly-description problem

U6, U7, U8, U9 and U10 have `Sensor_Temperature:DS18B20` symbols and value
`DS18B20`, but their assigned footprint is the through-hole connector
`Connector_JST:JST_XH_B3B-XH-A_1x03_P2.50mm_Vertical`.
Every one has pin 1 = power ground, pin 2 = shared GPIO18/1-Wire, pin 3 =
+3V3. These footprints cannot assemble a TO-92 DS18B20 as the named BOM
component. They represent external sensor interfaces, not five on-board
sensor packages. Cable/probe pinout remains an external contract.

Proposed bounded correction: represent U6..U10 as generic three-pin
connectors, identify them as DS18B20 interfaces in their values/descriptions,
and preserve every reference, pin number, net and assigned JST footprint.
Keep existing references for traceability; do not globally reannotate the
schematic. Verify exact pre/post connectivity and the five intentionally
changed BOM identities, then rerun ERC and DAC checks. This proposal has
not been implemented or approved in this review.

## Remaining assembly gates

- Select manufacturer/orderable parts, tolerances and ratings rather than
  treating generic values or footprint names as complete BOM entries.
- Verify every IC's actual pin functions and exact package variant; the
  all-component pad-number check is only the first structural gate.
- Confirm all external connector views, mating parts, cable pinouts,
  current ratings, retention and polarity markings.
- Inspect final populated-board clearances, orientation marks, stencil and
  fabrication limits on the future matched PCB. No matched v2 board exists.
