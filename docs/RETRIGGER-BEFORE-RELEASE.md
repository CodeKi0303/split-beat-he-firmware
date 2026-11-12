# C86 v0.3.37 release candidate: retrigger before release

This is prepared software with offline coverage. The new behavior has **not
been installed or measured on a physical keyboard**. Release readiness still
requires the complete build checks and explicit device/game validation.

## Behavior

- The option is per Hall key, OFF by default. It is stored in key `flags`
  bit `0x08`; clients must require INFO capability `0x40000`.
- With RT enabled, a partial upward movement starts a trough. Moving downward
  by `rt_press` from that trough requests another input without first reaching
  `rt_release`. The original release peak and thresholds remain unchanged.
- Each event resets the trough. Continued downward travel alone cannot repeat.
  Actual release and either deadzone clear the trough; bottom deadzone holding
  takes precedence. Turning RT off preserves the option but ignores it.
- Keyboard/chord, mouse-button and consumer mappings serialize an acknowledged
  held report, release report, and new held report. The physical key must still
  be pressed. Snapshot tags prevent an older completion from advancing a new
  phase. Relative scroll/pan emits one extra bounded delta instead.
- Other keys continue reporting. Duplicate HID usages remain union-held;
  entirely duplicated bindings cannot produce a separate repeat. Shared chord
  modifiers remain held while unique chord usages can repeat.
- One event per key may be pending. Excess requests coalesce. Actual release,
  output OFF, reconfiguration, stale sources and USB reset cancel pending work;
  a 100 ms timeout bounds waiting. Nothing enables output or saves settings
  implicitly.

The existing scan rate, ADC averaging, report intervals and mappings are not
changed. With no pending event, the optional ACK loop returns immediately and
auxiliary completion reads are skipped. USB transport retains a small snapshot
tag alongside each report; this is not a claim of zero CPU overhead or measured
unchanged latency.

## Compatibility and downgrade

Both firmware roles identify as **0.3.37**; key decisions run on the master.
Settings stay 476 bytes with unchanged endpoint/curve/deadband encoding.
C77/C85 and earlier C86 firmware reject the new flag. Before downgrading,
explicitly clear bit `0x08` on every key, save, and read back the saved profile.
Otherwise older firmware can skip the incompatible newest journal entry and
load an older generation, or start from defaults if none is compatible.

Sensor `pressed_mask` describes physical RT state, not the short synthetic HID
release. Sampled Studio/MCP sensor traces cannot prove that a game observed
those edges. Automatic RT fitting refuses profiles carrying this flag until
that behavior is modeled; no audio/game timing offset is optimized here.

## Offline evidence and required physical checks

The targeted C core suite and real USB callback tests pass. Coverage includes
both ADC polarities, ordinary RT and deadzones, fractional input, all mapping
kinds, duplicate usages/shared modifiers, delayed and stale completion tags,
mailbox overwrite, reset/off/stale cancellation, and bounded pending work.

Before normal use, verify on the target keyboard that partial return/repress
creates exactly one repeat, a long hold stays held, and actual release never
resurrects a key. Check keyboard/chord, mouse-button and consumer mappings in
the intended application. Distinct USB reports span host polls; games can
sample inputs less frequently and may miss a short release even when USB
acknowledged it. Firmware timing and game detection require actual measurement.

The wire and pulse contract is specified in [PROTOCOL.md](../PROTOCOL.md).
