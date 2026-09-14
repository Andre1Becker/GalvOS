# V2.0.6 buck feedback and bypass correction

Based on V2.0.5 commit `bf13b4b959f2df56b4617303c9e3c50e2a72675f`.
**Engineering draft, not approved for fabrication or laser operation.**

## Evidence and design scope

The earlier [power audit](2026-09-14-power-path.md) identified incomplete
component ratings and the 100 nF / 220 nF input-bypass discrepancy. The current
routed board additionally had 23.655 mm of FB-net copper distributed across
both layers with two vias. Its feedback divider was well away from the FB pin.

The manufacturer's LMR33630 datasheet, SNVSAN3F, was downloaded again from
<https://www.ti.com/lit/ds/symlink/lmr33630.pdf>. It matches the previously
inspected file: SHA-256
`3b0920a4e56a0b3f5f214a6317d3c06cff6e541e5054024adc31c30acc50c04a`.
Sections 9.2.2.6 through 9.2.2.8 specify the local ceramic capacitors;
section 10.1 calls for a close feedback divider, short FB/ground connections,
short/wide BOOT and VCC routing, and small input switching-current loops.

V2.0.6 addresses the local bypass specifications and feedback routing. It does
not qualify or redesign the entire input hot loop, output capacitor bank,
inductor, input protection, copper weight or thermal structure.

## Circuit and BOM

- C_INHF1: 100 nF becomes **220 nF, 50 V, X7R, 10%**, retaining its 1206 footprint
  and VIN-to-power-ground connection. The 220 nF/50 V/X7R specification follows
  TI's DDA guidance; it is not an added parallel capacitor.
- C_BOOT1: retains 100 nF/1206; specifies ceramic X7R, 16 V, 10%. TI requires
  at least 10 V. C_VCC1 retains 1 uF/1206 and specifies 16 V X7R, 10%.
- R_FB1/R_FB2 retain 100 kohm / 24.9 kohm, 1%, and their 1206 footprints.
  Layout requirements and explicit tolerance fields are recorded.
- No component is added or removed. All 122 components and 110 electrical
  net memberships are retained, with only C_INHF1's nominal value changed.
  Firmware, connectors, power topology, fan rails and safety logic are unchanged.

These fields specify procurement requirements, not selected capacitor MPNs.
Effective capacitance/bias/temperature, inductance/current ratings and actual
load response still require component selection and qualification.

## PCB change and preservation

| Item | V2.0.5 | V2.0.6 |
|---|---|---|
| Total FB trace centerline length | 23.655 mm | 6.550 mm |
| FB vias / layers | 2 / F.Cu and B.Cu | 0 / F.Cu only |
| R_FB1 center / orientation | (151, 51) mm / 0 degrees | (149, 48) mm / 90 degrees |
| R_FB2 center / orientation | (145, 51) mm / 0 degrees | (145.25, 44.5) mm / 180 degrees |
| BOOT/VCC trace widths | 0.25 mm | 0.40 mm |
| Board segments / vias | 1017 / 111 | 1012 / 108 |

The bottom feedback resistor returns locally to the exposed-pad ground.
The top resistor senses the existing output branch, away from the SW node;
the longer output-sense connection is not part of the high-impedance FB node.
The old ground spur, two FB vias and one ground via are removed. The short
BOOT/VCC paths retain their positions and receive wider copper.

Native comparison confirms 120 unchanged placements, all original footprint
types, pad dimensions/drills/shapes and pad nets. Of 1128 baseline copper items,
1108 retain exact geometry and width, 17 obsolete FB/ground/stub items are
removed, and three BOOT/VCC segments are widened. Nine new F.Cu segments stay
within x=143..152 mm, y=41..50 mm. Three named ground zones remain filled.
Only three reference labels are adjusted for the moved parts and bypass area.
No autorouter was used for this change.

The initial candidate left a 0.5 micrometer orphan stub and a reference-label
collision. These were corrected before adoption; no rule exclusions were added.

## Proof and limits

- Fresh export: unchanged net membership and exactly the approved value change
  against V2.0.5. The buck checker enforces the feedback ratio, BOOT/SW/VCC
  connectivity, power-ground separation, bypass values/packages and ceramic fields.
- Six interface checkers and 33 test methods pass. The five new buck methods
  include a valid case, old 100 nF rejection, changed feedback ratio, five wrong
  power/feedback/thermal-pad pin connections and an under-rated bypass capacitor.
- ERC: zero findings. Native DRC with refill, all-track checks and schematic
  parity: zero unconnected items, no errors, only the known buck footprint-type
  warning and two diode footprint-filter warnings. Draft guard passes.
- The project rule file is byte-identical to V2.0.5. The legacy PCB, firmware
  and KiBot workflow/configuration are unchanged. The front-layer detail was
  rendered and inspected. A relocated staged copy also passes export/ERC,
  all six interface checks, 33 tests, DRC/parity/draft guard and the native
  preservation comparison. Its KiCad source/project files are byte-identical.

See [CURRENT-HARDWARE.md](CURRENT-HARDWARE.md) for source hashes and commands.
To prove the electrical delta, also provide the V2.0.5 XML export to
`python hardware/tests/check_buck_interface.py AFTER.xml V205.xml`.

This is a layout/bypass improvement, not a power-supply release. Actual maximum
5 V load, 12 V fan currents, input range/fault behavior, capacitor/inductor MPNs,
effective capacitance, current capacity, input hot loop, thermal operation,
loop stability, EMC and physical testing remain open. The earlier audit's
10..30 V calculations are historical illustrations, not the currently confirmed
12.6 V nominal input contract. Independent Class 4 shutdown/rearm and all other
electrical/mechanical release gates remain unresolved.
