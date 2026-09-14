# Manual assembly, ESP32 pitch and SSR preselection

User update received after `1716ef8`: manual assembly by the user;
ESP32 pin pitch 2.54 mm; assistant to select the SSR; standard emergency-stop
button with two wires. These statements do not establish contact type,
row spacing, exact switched power branch or a complete safety chain.

## Accepted implementation facts

U1's schematic/PCB Assembly fields now specify manual assembly of two
1x22 female socket strips and user-confirmed 2.54 mm pitch. The local
footprint and placed footprint descriptions distinguish this confirmed
pitch from the still-provisional 22.86 mm row-center spacing and body
alignment. User-reported body remains 28 x 57 mm. No pads moved.

Manual assembly does not imply soldering-iron-only assembly. U_BUCK1 has
a concealed exposed-pad solder joint; paste plus controlled reflow/hot-air
or an appropriate assembly service is required to make that connection.
The fine-pitch packages also require appropriate tools and inspection.
No arbitrary thermal-hole/stencil redesign is inferred from “manual.”

## SSR candidate, not purchase or installation approval

Working assumption, presented to the user for confirmation: interrupt only
the nominal 12.6 V DC laser-driver feed, downstream of the supply split;
keep controller/buck and fans supplied. If the intended switch point is
the AC mains input instead, this selection does not apply. No mains routing
or wiring changes have been made.

Manufacturer-verified candidate:
**Omron G3NA-D210B-UTU DC5-24**, external panel-mount DC SSR with screw
terminals. The similarly named AC-output G3NA-210B is not interchangeable.
The DC-output variant also has an AC-input option; order the DC5-24 version.

Primary source: [Omron G3NA datasheet J166-E1-15](https://assets.omron.eu/downloads/latest/datasheet/en/j166_g3na_solid_state_relays_datasheet_en.pdf),
ordering table p. 2, ratings p. 4, characteristics p. 5 and terminal drawing
p. 9. PDF SHA-256:
`b903bb5c8ae99b42c645a991f7aa08d6ae9b7855b9a79aa9583a767195cd9da3`.
Manufacturer ratings table was visually checked, not inferred from search
snippets. Stock, price and current purchasing availability were not checked.

- DC output: rated 5–200 VDC, load-voltage range 4–220 VDC.
- Control: rated 5–24 VDC, operating 4–32 VDC, 5 mA maximum input current;
  must-operate threshold 4 V maximum.
- Nameplate 10 A is conditional: the datasheet specifies 0.1–10 A with
  its specified heatsink at 40 C, versus 0.1–4 A without. These are
  manufacturer limits, **not a GalvOS load/thermal qualification**.
- Output drop up to 1.5 V; off-state leakage up to 5 mA at the specified
  200 VDC test point. Do not extrapolate an exact leakage at 12.6 V.
  Driver undervoltage tolerance and OFF behavior remain unqualified.

### Existing control connection is not a verified match

The current NE555 output drives J_SSR1 through 330 ohms. Even an ideal
5.0 V source, with the SSR's permitted 5 mA input, leaves only
5.0 - 0.005 * 330 = 3.35 V at the input, below the 4 V guaranteed-operate
requirement. The real NE555 output cannot improve that upper-bound case.
Therefore this is **not a drop-in SSR for the existing J_SSR1 drive**.

A separately designed default-OFF transistor control stage powered by a
qualified supply is required before adopting this candidate. It must be
co-designed with the independent inhibit and rearm behavior. No stage has
been silently inserted and no 330-ohm resistor has been bypassed.

### Emergency-stop requirement

For a two-wire stop loop, require a latching button with a positively
opening NC contact: closed when released; open when pressed or on a broken
wire. “Two wires” alone does not prove those properties. Wire shorts and
SSR short-circuit failures are not detected or made safe by this one contact.

The current GPIO47 sampling accepts open/HIGH as OK; override can bypass
the stop decision. A normally-closed button must not simply be connected
and assumed safe with the current firmware. Required firmware logic and
the hardware shutdown architecture still need coordinated redesign.
A single ordinary SSR is not the sole safety-disconnection element for a
Class 4 laser: it can fail conductive and has off-state leakage. Independent
emission interruption and deliberate rearm remain mandatory design gates.

## Verification and preservation

Four text lines change across SCH, PCB and the local module footprint:
U1 Assembly in SCH/PCB and its two footprint descriptions. Full-file
comparison against the intended text replacements proves all other bytes
unchanged. Fresh XML subtrees for components, library definitions, library
links and nets match the previous canonical export after removing only
U1's changed Assembly metadata. All 122 components and 110 nets are retained.

ERC: zero findings. DRC/refill/parity: zero errors, zero unconnected items,
zero parity issues, one existing U_BUCK1 footprint-type warning. Seven
interface checkers and all 49 hardware unit tests pass. Project rules
unchanged. Refreshed canonical PDF; PCB renderings stay current because
all visible geometry is unchanged. No firmware/KiBot edits or Gerbers.

Current SHA-256:
- SCH: `8c1796266cc1843ee5e76cb9ab3a1025354a36771a9495bd96aa1e50c6ad0cc5`
- PCB: `b8111d778abc5b57ec3adb1bbc94060598fb87cc0d16e584e53e28c0bce4f47a`
- Local module footprint: `b6a6ffaeb4a56d3c07c6fcec29101eeb58c7d726a7576d771dc96b1c77a0ef10`
- PDF: `bbfc5fb4335a76f11f08f506cf9931459e7073332e4c70010572443b0927156e`
- PRO unchanged: `dc77f4155067018d81c509e667ad67642031a5cf69abe7181144c19c11bd3b1b`

Thermal/load/timing work remains excluded except for the essential
manufacturer limit disclosures needed to avoid misrepresenting the requested
SSR selection. No output-load qualification or production release.
