# Trigger-diode package identity — V2.0.10 draft

Date: 2026-09-14. Baseline: `d5296757928db13225d147827076c8460646e2ab`
(`hw-v2.0.9-draft`). **Not for fabrication or laser operation.**

## Cause and selected part

D_TRIGCL_SCAN1 and D_TRIGCL_WD1 used the axial `Diode:1N4148` symbol,
DO-35 description/datasheet and DO-35 footprint filter, but the PCB already
used `Diode_SMD:D_SOD-123`. That is an ambiguous purchasing/assembly
specification, not a warning to waive.

Reused the installed standard `Diode:1N4148W` symbol. Its inherited diode
graphics, pin positions, passive electrical types and numbering are the
same as the previous symbol. The two placed instances and PCB footprints
now explicitly select:

| Field | Both diode positions |
|---|---|
| Value | 1N4148W |
| Manufacturer | Diodes Incorporated |
| MPN | 1N4148W-7-F |
| Package / footprint | SOD123 / Diode_SMD:D_SOD-123 |
| Cathode | Pad 1, cathode band; respective timer TRIG pin 2 |
| Anode | Pad 2, MCU/timer power ground |

Manufacturer and orderable MPN are canonical schematic/PCB fields, not an
inferred substitution based only on the generic value. The selected part's
datasheet overrides the generic symbol catalog's default datasheet.

Primary evidence: [Diodes Incorporated DS30086, Rev. 31-2, September 2024](https://www.diodes.com/assets/Datasheets/ds30086.pdf).
Downloaded and inspected the five-page manufacturer PDF; SHA-256:
`39c16a6888bdab22418e93e17182174aad763a66957a4632e70c944194e3fc08`.
It explicitly lists 1N4148W-7-F, SOD123, 3000-piece tape/reel and cathode-band
polarity. Live stock and assembler sourcing have not been verified.
The Nexperia URL returned a challenge page and the catalog's Vishay URL
returned 404; neither is treated as verified manufacturer evidence.

## Package / polarity check

Both existing footprints are unrotated. Pad 1 lies left of the body center
and the existing silk cathode bracket is on that same side. The package's
cathode band must face this bracket; no diode or net was reversed.

| Geometry | Selected package | Existing PCB |
|---|---|---|
| Body | 2.55–2.85 x 1.40–1.70 mm; height 1.00–1.35 mm | SOD-123 footprint retained |
| Overall lead span | 3.55–3.85 mm | Pads centered at ±1.65 mm |
| Lead contact length / width | 0.25–0.40 / 0.52–0.62 mm | Each pad 0.90 x 1.20 mm |
| Suggested pad layout | 0.90 x 0.95 mm; overall pad span 4.05 mm | 0.90 x 1.20 mm; overall pad span 4.20 mm |

The installed KiCad pad layout is not identical to the suggested pattern.
With ideal centering and the stated package tolerances, each lead contact
lies within the rectangular pad envelope: minimum longitudinal toe/heel
allowance is 0.175 mm and minimum transverse side allowance is 0.29 mm.
These are geometric calculations, not solder-joint, placement-tolerance,
paste-process or assembly-yield qualification. No footprint, pad, courtyard,
position, rotation, mask/paste geometry or copper was changed.

## Electrical limits and remaining trigger risk

The selected manufacturer's limits at the specified conditions include
100 V DC/repetitive reverse voltage, 300 mA continuous forward current,
2 pF maximum capacitance at 0 V / 1 MHz, and 4 ns maximum recovery at the
documented 10 mA test conditions. VF is at most 0.715 V at 1 mA and 0.855 V
at 10 mA, at 25 C. These are device specifications, not measured circuit
performance or a board current rating.

In particular, the 400 mW / 315 C/W thermal figures use the datasheet's
large 2 x 2 inch anode/cathode copper pads and 3 oz double-sided board.
They do not describe these SOD-123 pads. Actual peak clamp current,
temperature, supply sequencing and trigger timing remain unqualified.

Both diodes are still **negative-only clamps**. They cannot limit positive
trigger excursions to the timer's supply. The [analog review](2026-09-14-analog-path.md)
already identifies scan loading, axis cancellation, insufficient trigger
sensitivity and modeled excursions above VCC; TI NE555 SLFS022K lists VCC
as the maximum trigger-input voltage. The watchdog's AC-coupled input also
needs complete waveform/current qualification. Correct package identity
does not repair or validate these circuits and does not establish an
independent laser shutdown function.

## Preservation and verification

- Same 122 components and footprint assignments; only the two generic diode
  values/identities change. All other values and all 110 net memberships
  match V2.0.9 exactly.
- All 122 placements/rotations, 400 placed pad records, 1115 copper objects
  and five zone blocks remain exact. All wires/labels and the diode symbol's
  pin geometry are unchanged. Board still has 1011 segments and 104 vias.
- Added hidden Manufacturer, MPN and Assembly fields. Updated the draft
  revision to V2.0.10; firmware, project rules, legacy PCB and KiBot unchanged.
- Native KiCad 10.0.6 ERC: zero. All-severity DRC, zone refill, all-track
  checking and schematic parity: zero errors, zero unconnected items,
  **zero parity findings**. Only U_BUCK1's existing thermal-hole footprint
  type warning remains. No native rule was relaxed or excluded.
- Removed the two resolved diode warnings from the draft checker's allowed
  warning set. A regression test rejects either warning if it returns.
- Seven interface checkers, 41 unit tests and the native DAC-source and
  buck-input/unique-ID guards pass. The new diode checker rejects V2.0.9,
  verifies the two allowed identity changes against that baseline, and
  checks actual manufacturer/MPN/datasheet, symbol identity and K/A polarity.
  New unit tests reject axial/wrong-package selection, reversed pads, wrong
  ground, wrong BOM fields, old symbol identity and swapped symbol pins.
- A relocated copy of the staged hardware repeats export, all seven interface
  checks, the exact V2.0.9 delta comparison, 41 unit tests, ERC, all-track
  DRC/parity and both native layout guards. All recorded artifact hashes
  match; user-owned untracked files are excluded.

## Reproduce

Run the current checkpoint's complete native ERC/DRC and interface test set,
including:

```sh
/usr/bin/python -B hardware/tests/check_trigger_diodes.py /tmp/galvos-current.xml
/usr/bin/python -B -m unittest discover -s hardware/tests -p 'test_*.py'
```

An optional second XML argument to the diode checker must be a V2.0.9
netlist; the checker permits only these two diode identity changes.

[Recorded hashes and checks](2026-09-14-diode-package-checks.json);
[current hardware checkpoint](CURRENT-HARDWARE.md).

Full production gates remain: independent Class 4 shutdown/rearm and RGB
reset-off, trigger/analog redesign decisions, DAC timing, actual loads and
buck thermal/stability/protection, ESP32 pin-grid/antenna/USB/enclosure fit,
complete BOM/assembly review and physical qualification. No Gerbers made.

After this verification, the user explicitly excluded further thermal, load
and timing work. Their existing findings remain unverified and documented;
they are not queued for further activity or treated as passed. This does
not change the diode package/polarity evidence or the remaining mechanical,
assembly and laser-safety work, and does not confer a release approval.
