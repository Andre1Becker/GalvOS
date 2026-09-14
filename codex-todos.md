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
