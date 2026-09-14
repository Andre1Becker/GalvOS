# Shutdown boundary review — 2026-09-14

**V2.0.7 remains a routing draft, not approved for fabrication or laser operation.**

This review corrects unsupported safety claims and records the implemented
shutdown boundary. It does not modify firmware, schematic, PCB, project rules
or KiBot. An existing external safety circuit may provide additional functions;
its presence, wiring and effectiveness have not been established.

## Audited baseline

Hardware commit: `b4385e3a3fff1313cbd545e9392028e87c34ad1a`,
tag `hw-v2.0.7-draft`. See [current hardware](CURRENT-HARDWARE.md)
for PCB geometry, prior ERC/DRC results and remaining warnings.

SHA-256 of the audited sources:

| Source | SHA-256 |
|---|---|
| V2 schematic | `e92f80b2c2eb07ac488f8748f1e45c8cb046f8f230c9b9100198b0ece4ccb37f` |
| V2 PCB | `8f4477ad75fe0f7b1899dd8e1304cf1b2b0bf4cbaac3f3813b198176316e91c2` |
| V2 project | `dc77f4155067018d81c509e667ad67642031a5cf69abe7181144c19c11bd3b1b` |
| `src/safety/safety.cpp` | `9b31677856e685004505bc0fad81e5df602963d77c1a7d5ca40d551c0aa899b2` |
| `src/safety/safety.h` | `dd11e8789a719ee8041625a845b65d37b38cefca5ece28e57a240eff89d60135` |
| `include/pinmap.h` | `d6bda116a6d631f1d99485a5187ec3d1900ed9de9981b21e98ec86a3be1f4835` |

## Schematic boundary

A fresh KiCad XML export contains 122 components and 110 nets. Relevant
memberships, confirmed from that export:

| Net / connection | Endpoints |
|---|---|
| `/GPIO47` | J_ESTOP1.1 → U1.J2_17 (GPIO47) |
| `/GPIO38` | U1.J2_10 (GPIO38) → U_WD1.4 (active-low reset) |
| `/GPIO14` | U1.J1_20 (GPIO14) → C_TRIGC_WD1.1 (watchdog trigger coupling) |
| `/WD_OUT` | U_WD1.3 → R_SSRCTRL1.1 |
| `/SSR1_CTRL` | R_SSRCTRL1.2 → J_SSR1.1 |
| `5V GND Buck` | J_ESTOP1.2 and J_SSR1.2; also U15.3 and U15.5 |

R_SSRCTRL1 is 330 ohm. U_WD1 is an NE555D. J_SSR1 is a control
connector, not an identified or qualified energy-interruption device.
No direct E-stop-to-SSR hardware gate is shown. GPIO38 controls the
watchdog timer's reset; a separate timer alone does not establish
firmware-independent enforcement of the E-stop or other status inputs.

U_SCAN1 monitors electrical command activity. It does not measure actual
mirror motion. Its buffered GPIO39 interface protects the MCU input but
does not qualify stationary-beam detection. See the
[scan-status review](2026-09-14-v2-scan-status.md).

## Firmware decisions and isolated reproduction

Sources: [safety.cpp](../../src/safety/safety.cpp), especially `init()`,
`allOk()`, `emergencyStop()` and `task());
[pin assignments](../../include/pinmap.h).

The [host decision probe](2026-09-14-shutdown-probe.cpp) embeds the two
decision functions verbatim and the E-stop sampling expression. GPIO,
logging, software-watchdog and subsystem status are stubbed. Assertions
reproduce the current behavior, including unsafe cases; they are
**diagnostic evidence, not desired safety requirements or a release gate**.

| Stimulus | Override disabled | Override enabled |
|---|---|---|
| ARM false, other inputs healthy | Enable decision false | Enable decision false |
| ARM true, one of E-stop/scan/software-watchdog/subsystem status false | Enable decision false | Enable decision true |
| That status becomes healthy again, ARM still true | Enable decision true without a new ARM request | Enable decision true |
| `emergencyStop()`, ARM initially true | Immediate GPIO38 LOW; ARM cleared; next decision false | Immediate GPIO38 LOW; ARM retained; next decision true |
| E-stop input HIGH / LOW | HIGH accepted / LOW rejected by sampling rule | Same sampling rule, but status bypassed in enable decision |

Each of the four status faults was tested separately. The input/decision path
in `task()` samples status and applies `allOk()` without clearing the ARM
request. Thus recovery of a status input can permit automatic re-enable even
with override disabled; a status trip is not equivalent to a call to
`emergencyStop()`. Other concurrent firmware actions can change ARM, but
the examined path does not itself enforce latched shutdown/manual rearm.

`init()` enables the E-stop pull-up. An open/broken wire that consequently
reads HIGH is accepted, not diagnosed as a trip. Initialization later writes
GPIO38 LOW; this does not prove OFF before initialization or during reset,
brownout or partial power. The task continues generating GPIO14 heartbeat
activity while running, including when override bypasses status conditions.

The nominal 20 ms task delay is not a measured or guaranteed shutdown time.
This probe is not a complete firmware build, scheduler/concurrency test,
electrical simulation, physical test or safety certification.

Reproduce from the repository root with a C++17 compiler (assertions enabled):

```sh
shutdown_probe_dir=$(mktemp -d /tmp/galvos-shutdown-probe.XXXXXX)
g++ -std=c++17 -Wall -Wextra -Werror -O0 \
  hardware/reviews/2026-09-14-shutdown-probe.cpp \
  -o "$shutdown_probe_dir/probe"
"$shutdown_probe_dir/probe"
```

This is a dated source snapshot. If firmware changes, compare the embedded
functions and sampling expression to the new source before interpreting it.

## RGB and thermal qualifications

For the stated active-HIGH laser driver and adequate optocoupler LED current,
GPIO HIGH → 6N137 output LOW → laser TTL OFF. A larger HIGH duty therefore
means more OFF time. A 10 kohm GPIO pull-up cannot guarantee the required
6N137 LED current; the maximum even before LED/series voltage drops is only
3.3 V / 10 kohm = 0.33 mA. Do not claim a reset-OFF guarantee from those
pull-ups. Shared input/output ground (e.g. U15.3 and U15.5) also prevents
claiming galvanic isolation merely because optocouplers are present.

[Temperature monitoring](../../src/sensors/temp_monitor.cpp), protection
action around lines 333–340, explicitly calls `safety::requestArm(false)`
for both alert and critical conditions. This action clears ARM regardless
of override; the retained-ARM finding above must not be generalized to it.
Temperature protection still depends on software and sensor qualification.

## Documentation correction and release decisions

README, introduction, troubleshooting and glossary now distinguish observed
monitoring behavior from unproven independent shutdown. Corrected claims
include RGB polarity, reset-OFF, optocoupler isolation, E-stop wire-break
handling, actual mirror sensing and the alleged software independence of
the timer chain. Historical design-intent comments in `safety.h` and
`pinmap.h` are not proof of an installed physical interlock chain; firmware
files and user-owned `agents.md` were not changed.

Before choosing or approving a safety circuit, obtain:

- Wiring/photo or circuit diagram of the existing external E-stop, key and
  enclosure contacts, including contact type and normal/failed states.
- Exact SSR/relay/power-switch model, terminals, supply and the rail it
  interrupts; how hazardous emission is inhibited if the switch fails.
- Any independent safety controller, shutter and actual mirror feedback,
  with their interface and fault-response specifications.

The resulting system must have a qualified firmware-independent,
default-OFF emission-inhibit path; defined response to wire breaks,
reset/brownout/partial power and relevant single faults; latched trip and
deliberate manual rearm; and qualified switching and scan-safety behavior.
Required shutdown time, diagnostics and architecture must be established
by the system risk assessment and independent competent review, not inferred
from this logic probe. No safety rating or approved replacement circuit is
claimed here.

Keep emission physically inhibited during bring-up. The remaining
[PCB plan](2026-09-14-v2-pcb-plan.md), power/thermal/mechanical work and
physical verification are still required. KiBot's separately proposed
[workflow correction](2026-09-14-kibot-ci.md) remains awaiting direction.

