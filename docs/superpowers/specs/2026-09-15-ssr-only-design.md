# SSR-only laser-driver enable — design for review

Date: 2026-09-15
Baseline: `09cec2c` on `main`
Status: **Architecture approved in chat; this written specification awaits user review.**
No circuit, PCB or firmware implementation is included in this checkpoint.

## 1. Scope and limits

The user approved switching only the nominal 12.6 V DC laser-driver branch.
Fans, controller/buck and galvo supplies remain outside that switched branch.
The user rejected an additional relay/contactor: use one external SSR only.

This implements an ordinary, single-channel functional inhibit, not a
single-fault-tolerant or certified Class 4 laser safety system. An SSR failed
conductive, a bridged stop loop or a failed control component can defeat the
inhibit. The architecture neither detects nor independently interrupts those
failures. User acceptance does not establish laser-operation approval.

Manual assembly and two female 1x22 ESP32 sockets are retained. Pin pitch is
confirmed 2.54 mm; 22.86 mm row spacing and body alignment remain provisional.
Do not change the encoder, USB/UART, DAC/SD buses, RGB polarity or other
unrelated interfaces to make room for this feature.

Thermal, output-load and timing qualification remain excluded from this work.
Consequently, no current rating, maximum shutdown time, driver undervoltage
margin or full production-release claim follows from this specification.
Logical sequencing and control-voltage compatibility still have to be correct;
excluded qualification must never be represented as passed.

## 2. Selected approach

Use a hardware-dominant stop path and a hardware restart interlock, followed
by the external SSR. A two-wire, latching, positively opening NC stop contact
interrupts the SSR's low-voltage control supply, not the laser load current.
Opening either wire therefore removes control supply without an ESP32 decision.

A software-only stop was rejected because it cannot interrupt a stuck MCU
command. A contact merely in series with the SSR input was also rejected:
reclosing it would restore enable when the old MCU command remains HIGH.
The additional electromechanical interrupt was offered and rejected by the
user; it is not reintroduced under another name.

The added restart interlock requires the command to be LOW after the hardware
conditions recover, followed by a new HIGH command. Faults dominate requests.
Manual intent is enforced through the application's explicit ARM action;
hardware cannot distinguish a deliberate MCU request from faulty MCU behavior.

## 3. Hardware ownership and interfaces

| Interface | V2 SSR-only meaning |
|---|---|
| J_ESTOP1.1 | Protected 5 V control-loop feed, not laser power |
| J_ESTOP1.2 | Return through the external NC contact; supplies SSR input positive and stop-loop sensing |
| J_SSR1.1 | NC-switched SSR input positive |
| J_SSR1.2 | Transistor-switched SSR input negative; no longer permanently grounded |
| GPIO38 | MCU enable request, LOW by default; held HIGH only for a valid current ARM request |
| GPIO14 | Existing watchdog heartbeat; remains active while normally disarmed |
| GPIO47 | 3.3 V hardware-ready feedback; HIGH in READY/ARMED, LOW in WAIT_RELEASE or with invalid supplies |
| GPIO39 | Existing scan-command status, not actual mirror-motion feedback |

GPIO40 is already used by `src/control/encoder.cpp`; do not repurpose it.
GPIO44 is a UART/module interface and is not needed by this design.
No new MCU pin is allocated.

The stop-return sensor must not feed 5 V into GPIO47. Use power-off-safe,
3.3 V-compatible logic with defined LOW defaults and no backfeed that can
sustain SSR input current after the NC loop opens. Contact conditioning must
not bridge the direct control-supply interruption.

Use a suitably rated, default-OFF transistor low-side driver. Its bias must
hold it OFF with the MCU disconnected and during invalid logic supplies.
Logic-supply and control-supply supervision reset the restart interlock;
recovery alone does not constitute a new ARM request. These are implementation
requirements, not claims already proved by the present hardware.

The watchdog's reset must no longer be gated by GPIO38: otherwise it could
never become healthy while the new interlock waits for GPIO38 LOW. Keep the
watchdog active under valid supplies while disarmed; its health output must
participate in the hardware reset condition. Preserve the timer's R/C network.
If the existing timer cannot provide the required health indication, stop
and report that incompatibility; do not bypass it or silently reinterpret
excluded timing work as successful qualification.

## 4. Hardware restart state contract

Define H = stop loop closed AND watchdog healthy AND control/logic supplies
valid. H is a hardware condition, independent of firmware and override.
The state machine is implemented in discrete logic, not the ESP32 task.

| State / event | Next state | SSR control command | GPIO47 |
|---|---|---|---|
| Any state, H false | WAIT_RELEASE | OFF | LOW |
| Power/reset initialization | WAIT_RELEASE | OFF | LOW |
| WAIT_RELEASE, H true, GPIO38 HIGH | WAIT_RELEASE | OFF | LOW |
| WAIT_RELEASE, H true, GPIO38 LOW | READY | OFF | HIGH |
| READY, H true, a subsequent GPIO38 HIGH | ARMED | ON | HIGH |
| ARMED, GPIO38 LOW, H true | READY | OFF | HIGH |
| READY/ARMED, H remains true with no command change | Hold state | OFF/ON respectively | HIGH |

A fault simultaneous with an ARM attempt wins. Reclosing the NC contact,
watchdog recovery or power recovery must not turn ON a request held HIGH
through the fault. A fresh LOW observation under healthy conditions is
required before accepting HIGH; an old LOW observed before the fault does
not qualify.

Component selection must demonstrate this state contract, including reset
priority and power sequencing; a bare level-sensitive AND gate is inadequate.
No numerical response-time guarantee is specified.

## 5. Firmware behavior and compatibility

Introduce an explicit V2-SSR-latch hardware build profile; do not silently
deploy the new connector/GPIO semantics to the tested V1 perfboard.
The old firmware and old harness are not compatible with the new interface.
In particular, grounding the old J_ESTOP return onto the new 5 V feed or
treating J_SSR1.2 as fixed ground defeats or damages the intended interface.
Board markings, wiring documentation and build selection must change together.

In the V2 profile, GPIO47 means hardware-ready, not raw button position.
A LOW can represent a pressed/open stop loop, a watchdog/supply fault, or a
restart interlock waiting for command release. Report “hardware inhibit”
rather than claiming to diagnose which physical contact opened. Do not use
an internal pull-up to manufacture a healthy reading.

The safety task owns the enable output and the pending ARM state. On startup,
DISARM, hardware-not-ready, a required software-health fault, emergencyStop,
reboot or update shutdown: drive GPIO38 LOW and revoke the current ARM request.
Faults discard previously accepted pending requests. Recovery and UI reconnect
must not restore ARM. Raising GPIO38 requires a new explicit ARM action after
readiness is observed; there are no automatic retries if hardware refuses it.

An asserted request that is not acknowledged by hardware-ready on the next
safety-task observation is cancelled and reported as an inhibit. Status must
distinguish a commanded enable from measured laser power: there is no
measurement proving that the SSR output is actually open or closed.

Override may bypass scan-command status only, consistent with the repository
safety rule. It must not bypass hardware-ready, software watchdog, registered
subsystem health, DISARM or explicit ARM. The hardware stop has no override
input. Scan-command sensing is still not mirror-motion sensing.

Retain the existing ARM/DISARM user interface; do not add another physical
button or network API without a separate justified design change. Prevent
automatic ARM from saved state, UI reconnect, repetitive state publication
or recovery code. A newly generated request from faulty/malicious software
is outside the single-channel hardware's ability to authenticate as human.

## 6. SSR selection boundary

Baseline candidate: **Omron G3NA-D210B-UTU DC5-24** with external screw terminals,
not the AC-output G3NA-210B or the AC-input variant.
The manufacturer source and limitations are recorded in
[the SSR review](../../../hardware/reviews/2026-09-14-manual-assembly-ssr.md).

Use the available 5 V control supply through the NC loop and the new driver;
do not retain the existing NE555 -> 330-ohm -> SSR direct drive. That path
does not guarantee the candidate's 4 V must-operate requirement.
Driver design must prove the input voltage/current envelope and OFF state
against the manufacturer's limits, including connector and transistor drops.

This candidate is not a purchasing or installation approval. Its output drop,
leakage and conditional current limits remain relevant. Their unresolved
compatibility with the actual laser driver prevents an unconditional claim
that it is suitable for the finished system. No SSR short-failure detection,
redundancy or galvanic laser-power disconnection is implied.

## 7. Required implementation proof

Acceptance concerns the functional inhibit only; all checks use a harmless
dummy load with laser emission physically inhibited.

1. Native schematic/PCB parity and net-delta audit prove only the approved
   control interfaces change; preserve other 122-component baseline functions.
2. NC-open and either broken-wire test remove SSR input drive with GPIO38
   forced HIGH, MCU stopped and firmware override enabled.
3. Closing NC again while GPIO38 stays HIGH does not restore drive.
4. LOW observed only before the trip does not permit restart; LOW after
   hardware recovery followed by a fresh HIGH does.
5. Watchdog-health loss uses the same reset/restart behavior; no startup
   dependency on GPIO38 HIGH is introduced.
6. Power/reset, MCU absent, supply loss and partial-power tests verify OFF
   defaults and no unintended backfeed. Recovery with stale HIGH stays OFF.
7. Fault/request coincidence tests demonstrate fault priority.
8. Firmware tests verify ARM revocation and no automatic recovery/reconnect
   rearm, with override both enabled and disabled.
9. GPIO47 tests cover every state-table row; no pull-up creates false readiness.
10. All existing interface checks, hardware tests and PCB placement/export
    coverage checks pass. New negative fixtures reject missing stop gating,
    stale-ARM restart, direct 330-ohm drive and override bypass.
11. ERC and DRC/parity are reviewed, not merely their exit codes. Existing buck
    footprint-type warning remains explicit; no new exclusions or relaxed rules.
12. Refresh schematic PDF, PCB views and assembly/wiring notes after the actual
    implementation; record build profile, hashes and hardware revision in Git.

These are future acceptance tests, not executed results for this design.
Bench behavior and expert system review cannot be substituted by netlist tests.

## 8. Review outcome and next action

Self-review checks: no unspecified switched branch, hidden second power
switch, automatic rearm requirement, borrowed encoder pin, or claim that the
SSR provides fault-tolerant isolation. Component-level proof belongs to the
implementation plan; failure to realize any state/voltage requirement is a
stop condition, not permission to weaken this contract.

The next step is user review of this written design. After approval, create
the implementation plan with component-level evidence and verification gates.
Until then, leave schematic, PCB, firmware, PDF and fabrication data unchanged.
