# GalvOS production PCB work log

## Objective and release status

Optimize `hardware/schematics/Laser Controllerv2_only_for_pcb_test.kicad_sch`
as the basis for a production PCB. **NOT RELEASED FOR MANUFACTURE.**
Existing PCB/Gerber files are legacy artifacts and are not evidence of v2 readiness.
Progress, verification and unresolved decisions are recorded here; changes are
versioned in Git. The pre-existing untracked `agents.md` belongs to the user.

## Acceptance checklist

- [x] Inspect the authoritative v2 schematic and run fresh ERC/netlist export.
- [x] Run fresh PCB DRC with zone refill; compare results with existing documentation.
- [x] Make custom libraries project-local and make the v2 project independently openable.
- [ ] Audit symbol pin types, package/pad mapping, values and BOM completeness.
- [ ] Verify input range/current, reverse polarity, fusing, transient protection,
  buck stability/thermal design, decoupling and power sequencing.
- [ ] Verify DAC/op-amp transfer function, bandwidth, output range and supply-fault behavior.
- [ ] Resolve independent laser shutdown, E-stop/interlock, watchdog, scan monitoring,
  reset/brownout/USB-only states and electrical logic levels against actual modules.
- [ ] Verify SD, DMX, fan and sensor interfaces, connector pinouts and cable protection.
- [ ] Confirm outline, mounting, connector positions and ESP32 antenna clearance.
- [ ] Produce a schematic-matched PCB with suitable return paths, current capacity,
  buck placement, thermal management, test points and readable silkscreen.
- [ ] Pass ERC, schematic/PCB parity and DRC without unexplained exclusions.
- [ ] Export reviewed BOM, assembly/placement, fabrication and electrical-test artifacts.
- [ ] Document bring-up and fault-injection tests; record measured results before release.

## 2026-09-13 — baseline audit and first schematic improvements

- Previous goal-turn classification: no prior goal work log exists in the current
  worktree. This turn establishes fresh evidence rather than assuming completion.
- Starting commit: `6bf007b5360fb4aac15ed3fce20ca96ec5048f54`; branch `main`.
- KiCad CLI: `10.0.6`.
- v2 ERC: 7 warnings, exit 5 with `--severity-all --exit-code-violations`:
  six custom-library resolution warnings plus the DAC LDAC pin incorrectly typed
  bidirectional against the ground power flag. No electrical safety conclusion
  follows from the absence of ERC errors.
- Existing `Laser Controller.kicad_pcb` DRC with `--refill-zones`: 327 violations
  and 197 unconnected items, exit 5. Includes 39 shorting items, 13 clearance
  violations, 48 solder-mask bridges and 2 invalid-outline findings. The old
  `LAYOUT.md` statements about a fully routed board and non-blocking findings
  do not describe the current file.
- Project and PCB still have basename `Laser Controller`; the target schematic
  has a different basename. Custom library tables point outside the repository.
- Fresh XML netlist confirms the fan power-input connectors J4/J6 are tied to
  the buck's +5 V output. Their permitted external use must be resolved before
  connecting another supply.
- GPIO39 is directly connected to the 5 V NE555 scan output. DMX connector RO
  is directly connected to GPIO4 while its module supply is +5 V. These require
  logic-level verification/protection for the ESP32-S3.
- E-stop currently reaches GPIO47 only; the SSR control comes from the watchdog
  NE555 output and its reset is driven by GPIO38. Independent E-stop/scan shutdown
  and default-off behavior are not yet established.
- Questions sent to the user: supply/current limits, mechanical/connector
  constraints and whether an external safety circuit exists.
- Raw baseline evidence currently in `/tmp/galvos-hw-audit.bGmdov/`:
  `erc-before.json`, `netlist-before.xml`, `drc-before.json`.

### Implemented and verified

- Added matching v2 `.kicad_pro` using default KiCad rules, with no exclusions.
- Imported the three assigned custom symbols and three assigned footprints;
  library tables now use `${KIPRJMOD}`. Initial normalized-text comparisons
  matched all six source files before the intentional DAC pin-type correction.
- Corrected DAC8562 LDAC/CLR/SYNC to inputs and VREFIN/VREFOUT to bidirectional
  in both embedded and local symbols, following TI's pin-function table.
- Added C9 (negative op-amp rail), C_WDVCC1 and C_SCANVCC1 (local NE555 supply
  bypass): each 100 nF / 50 V / X7R / 10%, with PCB placement fields.
- Updated the schematic title to engineering draft; corrected unproven
  isolation/default-off labels. Marked historical layout/Gerber claims clearly.
- ERC after electrical additions: zero violations, exit 0. Netlist comparison:
  101 components / 92 nets; all 98 original values/footprints and every original
  ref/pin connection preserved, with only the three expected capacitors added.
- Rendered the schematic and inspected all three added capacitor regions;
  adjusted the new NE555 capacitors to avoid existing text and sensor wires.
- Archived the staged schematic/project/libraries to a separate temporary
  directory and ran ERC there: zero violations, exit 0. This proves project
  library resolution is independent of the original checkout location.
- Final source hashes and machine-readable check summary are recorded in
  `hardware/reviews/2026-09-13-v2-checks.json`. Git whitespace checks passed.
- Full findings, manufacturer sources, layout/release gates and reproduction
  commands: [v2 audit](hardware/reviews/2026-09-13-v2-audit.md).
- Prior goal-turn classification for the next continuation: **progress** —
  authoritative schematic, project ownership and release documentation improved.
  The overall objective remains active, not achieved.

### Newly established electrical blockers

- DAC8562 at 5 V requires a guaranteed digital HIGH of at least 3.5 V.
- RGB boot pull-up current is below the 6N137 guaranteed input-HIGH requirement;
  10 kohm pull-ups do not prove laser OFF during reset.
- NE555 watchdog's AC-coupled 3.3 V edge into a 5 V pulled-up trigger has no
  guaranteed trigger margin; the basic timer is not a proven retriggerable
  missing-pulse detector. Scan sensing observes commands, not mirror motion.
- Independent safety architecture, supplies, actual external modules and
  mechanical constraints still await user information. Do not invent these
  constraints or label an untested design production-ready.

## Next actions

1. Qualify the implemented DAC level translation on the future PCB: source/PCB
   timing, rail limits, reset and partial-power behavior require measurement.
2. Resolve safety ownership with the user, then design and validate independent
   default-off gating, watchdog and interlock behavior against actual hardware.
3. Complete buck/input-protection, connector, fan/DMX and analog-range design.
4. Produce and verify the matched PCB after mechanical and electrical interfaces
   are settled; regenerate manufacturing outputs only after release gates pass.

## 2026-09-14 — DAC interface implemented; physical qualification open

- Previous goal turn: **progress**; commit `b769c0b` and its engineering-review
  tag are present. The only unrelated worktree file remains `agents.md`.
- Confirmed firmware uses SPI mode 1 at 40 MHz and actively drives GPIO13 as
  DAC /CLR. The current schematic leaves GPIO13 unconnected and pulls DAC /CLR
  to +3V3, so both voltage compatibility and reset connectivity need correction.
- Selected the standard KiCad `Logic_LevelTranslator:SN74LVC8T245`, strapped
  permanently A-to-B, with +3V3 on VCCA and the DAC's filtered +5V_ANA on VCCB.
  Manufacturer specifies partial-power-down protection, supply isolation and
  0.5–4.4 ns A-to-B delay for 3.3 V to 5 V over -40 to +85 C. The output supply
  follows the DAC supply to avoid driving a live 5 V signal into an unpowered DAC.
- Planned fixed startup bias: SYNC/CLR high, SCLK/DIN low on each side;
  unused inputs biased low, unused outputs no-connect, local supply bypass and
  series source termination. GPIO numbering and firmware frequency stay intact.
- Timing qualification must include source skew, pulse widths, CS high/hold
  times and PCB loading. A fast part alone is not proof of the 40 MHz link.

### Implemented and checked

- Added U_DACLV1 and its two local bypass capacitors, four 22 ohm source
  resistors, seven new 10 kohm signal-bias resistors and one unused-input bias.
- Reconnected GPIO13 to translated CLR. Existing R3 now pulls DAC CLR to DAC
  AVDD; its resistance and footprint are preserved.
- Native translator symbol/package pins checked against the TI table; direction
  is fixed A-to-B, OE is low, both VCCB pins and all three ground pins are wired.
- The new circuit and affected DAC/MCU drawing areas were rendered and inspected;
  labels and resistor fields were adjusted for legibility. Two displaced C_ANA1
  fields were returned to their component without changing its connections.
- KiCad ERC: zero violations, with all severities and no new exclusions.
- Export: 116 components / 104 nets including intentionally unconnected pins.
- `hardware/tests/check_dac_interface.py` passes the new export and rejects the
  baseline lacking a translator. The baseline comparison verifies every existing
  value/footprint and every unrelated net membership, not only component counts.
- Negative topology checks also reject a direct MCU-to-5V bypass and a wrong
  direction strap. Source hash and results: `hardware/reviews/2026-09-14-dac-checks.json`.
- Repeated ERC and the connectivity check from an archive of the staged files
  in a different directory: both passed; project/library relocation still works.
- Timing analysis leaves only 0.6 ns ideal SCLK pulse-width budget before source
  and PCB effects at 40 MHz. It is explicitly not a measured pass. Manual CS
  timing and complete partial-power behavior remain qualification requirements.
- Details and reproduction: [DAC interface review](hardware/reviews/2026-09-14-dac-interface.md).
- Previous-turn classification for the next continuation: **progress**. The
  overall production-PCB goal remains active; safety architecture, external
  specifications, matched layout and physical tests are still outstanding.

## 2026-09-14 — analog transfer and scan-coupling audit

- Continued from `68b3d4a`; preserved user-owned untracked `agents.md`.
- Verified 17 exact analog net memberships, 21 component values and ground
  connections on a fresh export. No schematic or firmware behavior changed.
- Including the 100 ohm DAC source resistors gives nominal
  `Vop = 5.462561881 - 2.178217822 * Vdac`. At center code the output is
  +17.017 mV. Default clamp endpoints are +5.190351 / -5.156150 V unloaded,
  outside the repository-stated +/-5 V galvo input range.
- The configurable 0.91 output scale helps nominal range where applied but
  is not a guaranteed voltage limit. Actual load, component tolerances and
  error budget remain unresolved; no calibration values were changed.
- Both 100 nF scan capacitors share the trigger node and couple the axes.
  The unclamped ideal RC model predicts 0.259470 V at the other axis node
  per 1 V DAC excitation at 10 kHz, and a 15.915 kHz differential-mode pole.
  Equal-and-opposite command motion cancels at the trigger with nominal
  matching. These are conditional model results, not hardware measurements.
- Positive command edges can drive SCAN_TRIG above its 5 V supply in that
  model; the fitted diode only limits negative voltage. NE555 upper-input
  protection and loaded DAC behavior are not established.
- Recorded resistor-only illustrative +/-1% corners, full RC equations,
  source references, model limitations and prototype tests in
  [analog audit](hardware/reviews/2026-09-14-analog-path.md); calculated
  values and source hashes in `hardware/reviews/2026-09-14-analog-checks.json`.
- Fresh KiCad ERC: zero violations. DAC connectivity/preservation checker:
  pass against the pre-translator baseline. Numerical RC checks cover DC,
  differential closed form, symmetry, cancellation and stable poles.
- Next: establish independent shutdown/actual mirror-feedback ownership
  before redesigning scan sensing; continue buck and interface audits while
  external contracts are unresolved. Do not disconnect the existing sense
  path as a waveform fix without a reviewed protection replacement.
- Progress is analytical evidence and explicit release gates. Complete
  analog qualification and the overall production PCB remain unfinished.

## 2026-09-14 — buck and power-path audit

- Continued from `9aa6ad5`; no schematic/firmware changes. Preserved `agents.md`.
- Checked eight exact power nets, eleven output-rail connections, twelve
  ground connections and thirteen values against a fresh KiCad XML export.
- J4/J6, labeled fan power inputs, directly share the buck output. Independent
  supplies there would be paralleled without isolation. D2 does not prevent
  output-to-regulator backfeed. Exact fan and DevKit/USB contracts remain open.
- Nominal feedback output is 5.016064 V; reference plus resistor corners give
  4.862490..5.173655 V before other errors. Worst rising EN corner is 9.409782 V
  after D2, leaving only 0.590218 V of startup headroom at 10 V raw input.
- At 30 V buck input, 5 V output, 400 kHz and 6.8 uH, nominal ripple is
  1.531863 A. An illustrative -20% L / 340 kHz case reaches 4.126370 A peak
  at 3 A load, above the 3.85 A minimum high-side current-limit threshold.
- Nominal local 44 uF output capacitance lacks bias/tolerance and load-step
  qualification. For an illustrative 2 A / 250 mV step, TI equation 6 gives
  about 50.867 uF effective at 30 V. This is not an assigned load requirement.
- C_INHF1 is 100 nF versus TI's local 220 nF DDA input bypass recommendation.
  L1/capacitor MPNs and ratings, input protection, reverse-energy handling,
  thermal/layout design and physical measurements remain release gates.
- Asked for actual source voltage and 5 V consumers, especially fan currents.
  No unconfirmed requirements or replacement component values were adopted.
- Evidence, formulas, qualifications and next steps:
  [power audit](hardware/reviews/2026-09-14-power-path.md) and
  `hardware/reviews/2026-09-14-power-checks.json`.
- Fresh ERC: zero violations at all severities. DAC connectivity/preservation
  checker passed; saved power calculations and source hashes were verified.
- Overall production-PCB work remains unfinished; continue independent
  interface checks while awaiting power and safety contracts.

## 2026-09-14 — package and assembly structural audit

- Previous goal turn: **progress**, with power-path evidence committed as
  `03071ec`. Existing user-owned `agents.md` remains untouched.
- Fresh XML audit: all 116 components resolve to 21 existing footprints;
  numbered pad sets match exported symbol pin sets for every component.
  This does not complete pin-function, package or assembly qualification.
- Checked custom DAC/OPA pitch, pad dimensions and numbering against TI
  package/land examples. Dimensions differ, but no replacement was justified
  merely by that difference; native generic alternatives also differ.
- Distinguished the buck footprint's extended ground copper from its
  package exposed pad and separate mask/paste geometry. Thermal assembly
  qualification remains open. Recorded DevKit header geometry, not a claim
  that an unspecified actual module fits or has adequate antenna clearance.
- Found an actionable BOM mismatch: U6..U10 use DS18B20 sensor symbols/values
  but JST three-pin connector footprints. All five are wired GND/1-Wire/+3V3.
  Proposed generic connector representation preserving refs, pins, nets and
  footprints; implementation awaits the bounded-design approval checkpoint.
- Details and limitations:
  [package audit](hardware/reviews/2026-09-14-package-audit.md).
- Overall objective remains unfinished; this audit is not a manufacturing
  release and does not settle power, safety or external connector contracts.

## 2026-09-14 — external GPIO contract audit

- Previous goal turn: **progress**; package/BOM audit committed as `40585b3`.
  Automatic continuation is not approval of the pending sensor-symbol fix.
- Compared 17 schematic endpoints with numeric firmware pin definitions:
  15 match, two do not. FAN1_TACH actually reaches GPIO9 and FAN2_TACH GPIO2,
  opposite `PIN_FAN1_TACH=2` / `PIN_FAN2_TACH=9` and repository instructions.
- Reproduced both mismatches from exported MCU pin functions. Tach acquisition
  is not implemented, so this is a latent channel-identity error, not a
  demonstrated current RPM failure. PWM and separate SD/DAC assignments match.
- Proposed swapping only the two schematic MCU tach destinations, preserving
  connectors, pull-ups, PWM, firmware and all values/footprints. Pending design
  approval alongside the five sensor connector/BOM corrections.
- Recorded SD/DMX/fan/sensor/RGB connector maps and qualification gaps in
  [external-interface audit](hardware/reviews/2026-09-14-external-interfaces.md).
  In particular, fan firmware comments describe MOSFET modules while v2 uses
  direct four-pin fan headers; actual intended hardware is still required.
- Clarified safety override precisely: it bypasses E-stop/watchdog/subsystem
  checks but retains the user-arm request. No firmware safety change made.
- The overall objective remains active and unachieved; no release or physical
  interface qualification is implied by the successful GPIO matches.

## 2026-09-14 — blocked checkpoint after repeated approval requests

- Previous goal turn: **progress**, with the GPIO contract audit in `6867729`.
  This continuation adds no implementation progress; it revalidates blockers.
- Current worktree has no new schematic, firmware, specification or layout
  changes; only the pre-existing user-owned `agents.md` is untracked.
- The same design-approval dependency has persisted through three consecutive
  goal turns: package/BOM review (`40585b3`), interface review (`6867729`), and
  this checkpoint. Automatic goal messages have not supplied the requested
  explicit approval. The brainstorming skill's implementation gate remains.
- Safe independent reviews have identified the next concrete corrections.
  Repeating those reviews is not a substitute for implementing them, and no
  live external process is being awaited. Stop automatic work as blocked,
  not complete, until user input permits meaningful next action.
- Immediate resume condition: approve representing U6..U10 as sensor
  connectors and correcting FAN1_TACH to GPIO2 / FAN2_TACH to GPIO9, with
  the preservation checks described in the existing review proposals.
- Wider release dependencies remain: actual source/5 V load and fan data;
  external module/connector contracts; independent Class 4 laser shutdown
  ownership; board outline, mounting, connector positions and antenna space.
  No matched v2 PCB or measured qualification evidence has been supplied.
- The production-ready PCB objective is unchanged and unachieved. Blocked
  status is an input checkpoint, not a hardware release or abandoned scope.
