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

1. Specify and implement guaranteed DAC level translation while preserving
   analog range; verify reset and partial-power behavior.
2. Resolve safety ownership with the user, then design and validate independent
   default-off gating, watchdog and interlock behavior against actual hardware.
3. Complete buck/input-protection, connector, fan/DMX and analog-range design.
4. Produce and verify the matched PCB after mechanical and electrical interfaces
   are settled; regenerate manufacturing outputs only after release gates pass.
