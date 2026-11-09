# Keyboard protocol v1 — C77 / C85 / C86

The explicit C86-left v0.3.10 `--adc-dual64` candidate uses ADC1/2 at64kHz,
12-bit,30MHz with one publication/IRQ per scan. The local eight-sample running
average uses the same integer units as v0.3.6. INFO reports64000; settings,
STATUS, USB/S7 layouts and existing host codecs are unchanged. No live hardware
upgrade is implied by this version. See [64k dual ADC](docs/C86-64K-DUAL12.md).

The explicit C86 left v0.3.9 96kHz candidate changes private acquisition
and decision data: native14 samples are averaged in Q5 legacy12-bit units.
INFO reports96000 for that image. Existing configuration records, STATUS.raw
(rounded unaveraged12-bit units), STATUS.travel, USB and S7 formats do not
change; the standard64k and16k profiles retain their old acquisition paths.
No host codec or simulation claims to expose native14 ADC samples. See
[96k design and validation boundary](docs/C86-96K-14BIT.md).
Version0.3.8 also adds the read-only ADC diagnostics command0x42 described below.
Version0.3.9 batches two DMA frames per interrupt (48kHz publication) while
retaining all96k samples in the eight-sample average, calculated in foreground.
It adds read-only scheduling diagnostics0x43; acquisition rate is not the
foreground RT decision rate or USB rate. Building does not update hardware.

Implementation contract. All integers little endian. Development VID/PID 0483:5744; these are not allocated product IDs. Vendor HID usage page FF60, usage 61, report ID 0, 64-byte input/output. Only the LEFT master exposes USB; right is a raw sensor satellite. C86v0.3 adds a bounded application updater below; no arbitrary-address memory/flash operations.

INFO `board_rev` identifies the explicitly compiled physical revision: 77, 85, or 86. Python and JavaScript clients accept only those revisions with the same 14-Hall/6-digital/4-encoder/28-input layout and protocol 1; unknown revisions, roles, or layouts are rejected. The saved/exported `c77-profile-v1` / `board: C77` schema still names this shared key-layout format, not the physical PCB revision. Existing profiles remain compatible with the new firmware. The demo models C86 configuration firmware0.3.5 (no updater or performance counters). Firmware selection and flashing are never inferred from protocol compatibility.

USB interfaces: 0 keyboard (32-byte bitmap; bit n is HID keyboard usage n, including modifier usages E0..E7); 1 mouse (u8 buttons bits0..4, s8 X=0, s8 Y=0, s8 wheel, s8 pan); 2 consumer (little-endian u16 usage); 3 vendor control (64 bytes). Firmware0.1 used a two-byte wheel/pan-only mouse report; firmware0.2 changes that descriptor and bumps bcdDevice to0x0200. Firmware0.3.1 adds neutral X/Y usages for Windows mouse-driver compatibility, expands that report to five bytes and sets bcdDevice to0x0301. All reports have report ID 0 (no ID prefix on the wire). Keyboard is always NKRO, with no six-key fallback or toggle; duplicate bindings merge by usage and remain held until all contributing sources release. Keyboard is a non-boot HID interface, so BIOS boot-protocol keyboard support is not promised. Keyboard endpoint 0x81 has bInterval=1: 125us at high speed, 1ms at full speed. Vendor interrupt endpoints 0x84/0x04 are 1ms at both speeds. Mouse/consumer updates run at1ms; relative deltas are never replayed by GET_REPORT/idle, while held mouse buttons are preserved. Enumeration and physical 8kHz operation require validation on the actual PCB.

INFO capability bit5 (`0x20`, `CAP_MOUSE_BUTTON`) means mouse-button bindings and Hall/MX press-edge scroll are supported (firmware0.2+). Bits0..4 retain the legacy capability mask. Clients reject unsupported bindings and complete-profile imports before the first mutation on firmware0.1. Existing calibration/RT/keyboard/consumer/encoder-scroll work with firmware0.1. A profile containing new bindings must not be restored to older firmware; an older firmware image rejects such stored configuration rather than interpreting unknown kinds.

INFO capability bit8 (`0x100`, `CAP_BOTTOM_DEADZONE`) means Hall bottom deadzone is supported (firmware0.3.4+). Clients require that bit before any nonzero bottom-zone SET_KEY; complete-profile import preflights every key and binding before OUTPUT or RAM writes. Zero remains compatible with older firmware. The existing 22-byte key record uses formerly reserved bytes16..17, so the settings payload remains476bytes and the journal version/CRC layout are unchanged. Previously saved zero-reserved records load with bottom_deadzone=0 and retain calibration/mappings. JSON profiles may omit `bottom_deadzone` and/or `curve_id`; validation supplies0 and new exports include both. Older firmware rejects saved nonzero bottom-zone records; reset all bottom zones to0 and explicitly save/verify before a downgrade if settings need to remain compatible.

INFO capability bit9 (`0x200`, `CAP_HALL_CURVE`) means per-key Hall reference curves are supported (firmware0.3.5+). Key record bytes18..19 contain `curve_id` (0=linear legacy, 1=Owlab Ti reference approximation); bytes20..21 remain zero-reserved. Clients must preflight capability before a nonzero-curve write, including whole-profile imports. Unknown curve IDs are rejected. Old settings keep curve0, raw calibration endpoints, bindings, and thresholds: an update does not silently enable a new curve. A nonzero curve changes the coordinate in which AP, RT and both deadzones are evaluated. Changing curve releases input and disables output like every SET_KEY; selecting it is an explicit configuration action. Before downgrading, set all curve IDs to0 and explicitly save/verify; old firmware rejects stored nonzero-curve records.

Downgrade detail: the journal selects the newest record that its running firmware can decode. An older firmware can skip a new nonzero-curve record and load an older compatible generation, restoring stale settings; if no compatible record remains it starts from defaults with output disabled. To preserve the intended current profile on a return to0.3.3, explicitly zero both curve IDs and bottom deadzones, save and verify that compatible latest generation first. Updater rollback is not automatic.

Packet: bytes 0..1 ASCII C7, byte2 version1, byte3 opcode, bytes4..5 nonzero transaction u16, byte6 status (request=0), byte7 payload length 0..54, bytes8..61 payload followed by zero padding, bytes62..63 CRC16-CCITT-FALSE (init FFFF/poly1021) over bytes0..61. Reply opcode=request|80; same transaction. No unsolicited reports. A host serializes requests and discards unmatched replies. Firmware validates length, CRC, padding, key/range values before mutation. All configuration mutations release input and disable output. Changes are RAM-only until explicit SAVE. First boot is uncalibrated/output disabled.

Statuses: 0 OK,1 BAD_COMMAND,2 BAD_PACKET,3 BAD_INDEX,4 BAD_CONFIG,5 BUSY,6 CALIBRATION_INVALID,7 NOT_READY,8 STORAGE_ERROR.

| Op | Name | Request payload | Reply payload |
|---|---|---|---|
|1|INFO|empty|u8 major=0,minor=3,patch=9,role=0(left); u8 hall=14,digital=6,encoders=4,inputs=28; u32 scan_target=96000(C86 explicit left experiment)/64000(C86 default)/16000(recovery or legacy); u32 usb_interval_us=125HS/1000FS; u32 capabilities; u32 generation; u32 config_crc; u16 flash_KiB; u8 board_rev=77/85/86 (explicit build); u8 protocol=1; u8 usb_speed(0none,1FS,2HS); u8 storage_valid; remaining reserved|
|2|STATUS|u8 side(0left/1right)|u32 uptime_ms; u32 sample_sequence; u32 errors; u32 link_age_us; u16 raw[7]; u16 travel[7]; u8 digital_mask; u8 flags(bit0output,1source_fresh,2dirty,3stored_valid); u8 cal_key(255idle); u8 cal_state(0idle,1collect_rest,2rest_ready,3collect_press,4press_ready); u32 pressed_mask; u16 reserved=0 (54 bytes)|
|3|GET_KEY|u8 key0..13|key record22bytes below|
|4|SET_KEY|key record22bytes|same canonical key record|
|5|CALIBRATE|u8 key,u8 action(0capture_rest,1capture_press,2commit,3cancel)|u8 key,u8 state,u16 rest,u16 pressed,u16 count; requires 256 fresh stable samples at each stop, span>=128; capture timeout5s|
|6|OUTPUT|u8 enabled(0/1)|u8 actual; enable requires fresh local/remote sources and every enabled Hall key calibrated; USB disconnect or stale source releases all keys|
|7|SAVE|empty|u32 requested_generation; accepted only, NOT persisted proof. Capture output state, release output, finish reply, detach USB, stop scan/link, write journal and reboot. Host reconnects and checks generation, config and live output.|
|8|DEFAULTS|empty|empty; RAM only|
|9|GET_BINDING|u8 input0..27|binding6bytes below|
|10|SET_BINDING|binding6bytes|same canonical binding|

INFO capability `0x4000` means SAVE persists the live output setting. The
journal stores it in CRC-covered header byte 16 (0 off, 1 on). Zero-reserved
records load with output off. On boot, a saved-on device waits for configured
USB, fresh data from both halves and valid calibration for all enabled keys,
then enables once. Explicit output-off and config edits cancel pending startup
enable. Saving with output off writes a record readable by older firmware.
Older firmware skips records with byte 16 set. A C86 firmware update initiated
from a supporting app suppresses auto-enable on its first application boot.

Key record22bytes: u8 key;u8 flags(bit0enabled,bit1RT,bit2calibrated);u16 released;u16 pressed;u16 actuation;u16 rt_press;u16 rt_release;u16 top_deadzone;u16 noise_deadband;u16 bottom_deadzone;u16 curve_id;u16 reserved=0. ADC0..4095. Travel0..1000 is a normalized coordinate, not millimetres: curve0 reports normalized ADC range; curve1 reports an approximate normalized distance from the reference curve. top_deadzone<actuation<=1000; 0<=bottom_deadzone<=1000-actuation; RT distances1..1000; noise0..100; curve_id0..1. Uncalibrated equal endpoints allowed, calibrated span>=128.

Curve1 uses the public NeonHE Tab60 HE `OW Ti` reference (downloaded2026-11-07), not the provisional C86 caliper coordinates or a measured C86 distance characterization. Signal nodes are `[0,467,1211,2244,3667,5644,8500,10000]`, axis nodes `[0,5000,10000,15000,20000,25000,30000,33000]`. Treating the public `pcnt` nodes as a normalized ADC fraction is an approximation; NeonHE's firmware normalization is not verified. Per-key polarity-corrected distance `d` and span `s` clamp at0/full. For0<d<s: signal=`floor((d*10000+floor(s/2))/s)`; choose the containing signal segment; axis=`axis0+floor(((signal-x0)*(axis1-axis0)+floor((x1-x0)/2))/(x1-x0))`; travel=`floor((axis*1000+16500)/33000)`. Linear mode retains `floor((d*1000+floor(s/2))/s)` exactly. Both motion directions use one monotonic curve; no hysteretic curve switching or extrapolation. Host mm=`travel*total_travel_mm/1000`. Owlab Ti HE uses the user-supplied nominal3.4mm total travel, proportionally rescaling the source3.3mm curve. Another stroke reuses this shape scaled by its own total travel and remains an approximation; different magnets/geometries can change the shape. UI0.01mm increments do not establish physical0.01mm accuracy. Settings do not save total-travel metadata to device Flash; Studio profiles retain it.

Curve processing is master-only. The satellite still sends raw ADC in unchanged S7 protocol1 frames with no app-version handshake, so a left0.3.5 app can process a right0.3.3 satellite. Only the left app needs updating for this feature. This is source/protocol compatibility, not a substitute for post-update live link/performance validation. No permanent bootloader update is required.

The explicit C86 right v0.3.20 96 kHz independent DMA profile publishes a
rounded average of its latest eight completed conversions per Hall key in
the same seven 12-bit S7 Hall fields. Older satellites still send the most
recent unaveraged codes. On this right profile, `STATUS.raw` and calibration
consume the transmitted averaged ADC codes; the left retains unaveraged
`STATUS.raw` and its separate decision average. Packet layout, ADC code
range, settings, and master-side curve/deadzone/RT processing are unchanged.
The right S7 source sequence advances with the slower consumed DMA cursor.
It is distinct from the 8 kHz RS-485 frame counter. See
[independent DMA profiles](docs/C86-ADC-ASYNC64.md).

C86 left v0.3.6 adds an acquisition-stage moving average before the existing ADC deadband and Hall curve/RT processing. It uses8 consecutive complete ADC scans at64kHz, or2 scans in the separate16k recovery build. It does not change ADC sample time, scan target, USB interval, configuration records, or the S7 wire packet. `STATUS.raw` and calibration still use unaveraged ADC samples; `STATUS.travel` is the firmware decision coordinate after filtering, deadband and curve. Live UI millimetres must scale `STATUS.travel` rather than remap `raw`. The optional averaged values exist only in the internal C sample structure; no host codec fields are added. Right/legacy samples retain the previous input path. Only the left app needs updating; the existing right0.3.3 app remains protocol-compatible. These facts describe prepared software, not measured post-update hardware quality. See [filter design and limits](docs/HALL-FILTER.md).

Bottom deadzone0 disables the feature and preserves prior RT behavior. With RT on and bottom_deadzone>0, normalized travel at/above `1000-bottom_deadzone` holds the key pressed and anchors the RT peak at that boundary. Moving just above the boundary does not itself release: release requires an additional `rt_release` upward travel from the boundary. Below it, normal released-trough/repress logic applies; re-entering the bottom band holds pressed again. This prevents bottom-depth jitter from releasing/repressing and avoids a discontinuity when leaving the band. For example bottom100 and RT release50 holds through travel900..1000 and releases at850. The top deadzone still resets the cycle; disabled keys stay released. With RT off the ordinary actuation threshold is unchanged. STATUS travel always reports the actual normalized sample, not a deadzone-remapped depth. Only Hall keys support RT/deadzones; MX and encoders do not.

Binding6bytes: u8 input;u8 kind(0disabled,1keyboard,2vertical_scroll,3horizontal_scroll,4consumer,5mouse_button);u16 code;u8 modifiers;u8 reserved=0. Keyboard usage4..DF orE0..E7 (0 means modifiers only); modifiers8bit. Scroll code signed16 -1 or+1; encoder directions use individual bindings. On Hall/MX inputs0..19, scroll emits one tick per press edge (including Hall RT re-press), with no timed repeat while held. Consumer allowed: B5(next),B6(previous),B7(stop),CD(play/pause),E2(mute),E9(volume+),EA(volume-). Mouse button code1left/2right/3middle/4back/5forward: Hall/MX follows hold/release; encoder directions generate bounded2ms click pulses. Nonkeyboard modifiers=0. Mouse buttons and Hall/MX scroll require capability0x20. Encoder RT is never supported; Hall RT controls only Hall inputs0..13.

C86 v0.3.22+ with INFO capability `0x2000` (`CAP_KEY_CHORD`) also accepts
kind6, encoded as `[input,6,key1,key2,modifiers,key3-or-0]`. Ordinary
HID usages are `0x04..0xDF`, distinct and strictly ascending. The JSON
record is `{input,kind:6,keys:[key1,key2,...],modifiers}`. Single keys,
modifier-only bindings and single-key chords still use kind1. The master
reports all selected usages together in the existing NKRO bitmap. A usage
shared by overlapping bindings remains held until the last source releases
it. For an encoder pulse, press and release acknowledgments check the full
chord; no report order or keyboard macro is implied. Mouse and consumer
bindings remain separate kinds. Profile stride and journal size are unchanged;
older firmware cannot load a generation containing kind6. Export the chord
profile and save a compatible mapping before an intentional downgrade.

Inputs0..6left Hall,7..13right Hall. Each side Hall order CAPS,Q,W,E,R,SHIFT,SPACE (labels describe left physical positions; right mirrored). 14leftMX1,15leftMX4,16leftknobclick,17rightMX1,18rightMX4,19rightknobclick. 20leftrollerCW,21CCW,22leftknobCW,23CCW,24rightrollerCW,25CCW,26rightknobCW,27CCW. Encoder detent direction is provisional until hardware test.

MCP is a PC-side stdio server using the exact same vendor HID commands. Require a device serial selector for real hardware and an explicit write-enabled launch option for mutations. Demo/simulated mode is labelled and never opens HID. Do not collect ordinary PC keystrokes. Profiles contain calibration/bindings only. No arbitrary shell, firmware upload, or background writes.

## C86 v0.3 performance and app update extension

USB app bcdDevice is0x0304 (permanent boot remains0x0303). INFO capability0x40 means C86 permanent-updater protocol support in this app;0x80 means local performance counters. They do not prove a compatible bootloader is installed: UPDATE_INFO flags are authoritative. C77/C85 capabilities are0x33F; C86 is0x3FF, including bottom deadzone0x100 and Hall curve0x200. The dedicated updater bypasses ordinary app INFO so recovery boot can be reached. MCP configuration tools do not expose these flash commands.

`0x40 PERFORMANCE`, empty request, reply52bytes =13 little-endian u32:
device_micros, keyboard_IN_completed, keyboard_published, submit_failures,
stale_neutralizations, ADC_sequence, ADC_errors, ADC_overruns, ADC_late_irqs,
ADC_min_period_cycles, ADC_max_period_cycles, ADC_max_IRQ_cycles, scan_target_hz.
Cycles use240MHz CPU. Extrema are since ADC start, counters may wrap/reset; compare with elapsed time. These are device-side counters, not Windows/wire measurements.
For v0.3.9 explicit96k, sequence advances by2 per published DMA block; period
extrema describe the48k block-publication interval, not the10.4167us ADC trigger.
Compute acquisition rate from sequence delta/time, never reciprocal block period.
In the independent DMA trials (v0.3.18 64 kHz and v0.3.19 96 kHz), ADC sequence is the slower completed DMA cursor,
period extrema are ADC1 half-ring IRQ intervals, and local updates in
`0x43` count foreground iterations that consumed at least one new bank.

`0x42 ADC_DIAGNOSTICS` (C86 left app0.3.8+, capability0x80), empty request,
reply52bytes =13 little-endian u32: running, sample_sequence, errors, overruns,
dma_errors, late_irqs, init_error, stop_reason, stop_phase_cycles,
stop_dma_flags, stop_ndtr1, stop_ndtr2, scan_target_hz.
This reads a snapshot without restarting or altering acquisition. Stop reasons:
0 none,1 completion flags/half,2 trigger epoch,3 DMA ownership,4 raw range,
5 filter,6 ADC overrun,7 DMA error. The first fault retains DMA flags/NDTR
before shutdown. Phase is unsigned stop-check cycles minus expected trigger;
v0.3.8 uses IRQ-entry time; v0.3.9 also checks the time after flag observation
and records that time for an observation-deadline failure. Its expected trigger
is the last scan of the two-frame batch.
a large wrapped value can mean entry before the expected epoch, not huge latency.
In the independent DMA trials, stop phase records cycles since the slower bank's
last serviced DMA boundary.
Init errors0..10 retain the ADC startup enum. Check running/sample progression
and absolute lifetime errors as well as interval deltas. Old firmware/demo must
be rejected before sending this command. No raw MCU address access is exposed.
Detailed stop-reason/phase/ownership fields are latched by the96k profile;
legacy16/64k paths may leave those fields zero even when their error counters
are nonzero. Reason0 alone is not evidence that acquisition is healthy.

`0x43 SCHEDULER_DIAGNOSTICS` (C86 left app0.3.9+, capability0x80), empty
request, reply52bytes =13 little-endian u32, in order:
device_micros, main_loops, main_loop_gap_max_us, local_updates, remote_updates,
cc_state, cc_error, link_flags, rx_bytes, uart_irq_max_cycles,
poll_irq_max_cycles, uart_irqs, poll_irqs.
Link flags bit0 active, bit1 allowed, bit2 transmitting, bit3 update_mode;
remaining bits zero. RX bytes count consumed bytes including invalid/update
frames. IRQ counts include inactive handler calls; elapsed cycles include
higher-priority preemption, not only that handler's CPU work. Main-loop gap
excludes the first entry. Updates count newly consumed source sequences,
not ADC triggers. Counter differences wrap at32bits; maxima are lifetime
values. CC state/error use the existing controller enum. These observations
are not an atomic electrical measurement or CPU utilization percentage.
Read this alongside0x40/41/42 and both STATUS replies before evaluating a
stale-source trial. The command never clears counters or restarts hardware;
MCP rejects old firmware and demo before sending it.

`0x44 ADC_TRACE` is available only on the C86 left 64 kHz comparison apps,
indicated by INFO capability `0x400`. Request: two distinct u8 local Hall
indices (0..6). Reply: 54 bytes, little endian. The header is u32 ADC sample
sequence, u32 sample timestamp in microseconds, and both requested key indices
as u8 in request order. Two 22-byte rows follow, in that order: eight u16 raw
ADC codes (oldest to newest), u16 rounded average, u16 ADC code retained
after deadband, and u16 travel/pressed (bits 0..9 travel 0..1000, bits 10..14
zero, bit 15 pressed).

The app accepts a trace only with output off, valid saved storage, no unsaved
settings and no calibration in progress. It replies from a single fresh
frame actually consumed by the engine, or NOT_READY if none is available
within 10 ms. No state is persisted; this is not a continuous ADC stream.

`0x45 CPU_PROFILE` is available only on C86 left comparison apps with INFO
capability `0x800`. Request `[0]` arms one bounded profile and returns a
nonzero u32 run ID. The app waits for the response to drain (up to 50 ms)
before starting a 60,000,000-cycle window at 240 MHz. Starting requires
output off, no pending save/calibration, valid saved settings and fresh
left/right input. Other vendor commands are refused while armed/running.
No settings, output or Flash are changed. A USB session change cancels it.

After waiting at least 350 ms from the ACK, request `[1,page]`, pages 0..4.
Page 0 is eight u32 in this order: run ID, elapsed cycles, CPU Hz, target
cycles, report publications, USB completions, total adjusted measured IRQ
body cycles, main loop count. Pages 1..4 are 44 bytes: u32 run ID, u8 count=3,
then three records each holding u8 stage ID and u32 calls/total/max cycles.
All multi-byte fields are little endian.

Stage IDs: 0 main service, 1 local snapshot/engine, 2 remote snapshot/engine,
3 tick/report/auxiliary reports, 4 vendor/housekeeping, 5 ADC IRQs,
6 split-link IRQs, 7 USB IRQ, 8 SysTick/I2C IRQs, 9 nonlinear normalization on
cache miss, 10 linear normalization on cache miss, 11 valid RT calls (count
only). Normalization can clamp at the endpoints and is nested in the engine;
do not add stages 9–11 to the major time buckets. DWT spans subtract completed
instrumented IRQ bodies, but observer overhead and uninstrumented time remain.
Totals do not represent generic CPU utilization or end-to-end input latency.

`0x47 ADC_TUNING` and `0x48 ADC_COMPARE` require capability `0x8000` on
the C86 left paired-sampling app. `ADC_TUNING` reads with an empty payload or
applies a 5-byte `u32 rate_hz, u8 window` payload. Rate choices are 8000, 16000,
32000, 64000 and 96000. Window choices are 1 (latest scan), 2, 4, 6 and 8.
Window 0 restores the legacy defaults (R uses 8 frames, the paired keys use 1).
The response echoes the active pair. Changes require output off and no pending
output restore, calibration or save. An averaging-only change keeps acquisition
running and recomputes the latest completed window. A rate change re-arms DMA
and TIM3 with the existing ADC configuration. Both invalidate old engine
snapshots and clear RT history. The choice lives in
RAM and is independent of USB polling and the saved calibration profile.

`ADC_COMPARE` takes a u8 local Hall index (0..6). It returns the key index,
effective window, u32 bank sequence, u16 latest ADC code, u16 moving average
and u16 post-deadband decision code (12 bytes total). All are little-endian
and from the same engine snapshot. The latest code for a phase-paired key is
already the mean of its two conversions within that scan. This is an opt-in
read-only snapshot and does not add traffic to the normal S7 link.

`0x49 ADC_PAIR` requires capability `0x10000` and takes the same five-byte
SET payload. With output off and a fresh satellite, the master pauses S7 and
relays one C7 request. The satellite validates the setting, applies the window
or rate and echoes the active values. The master applies its setting only after a
matching acknowledgement, then resumes S7. The acknowledgement is not an
atomic commit. If delivery or a restart is uncertain, output stays off
until a successful paired request. Normal S7 packet length is unchanged.

`0x46 ADC_IRQ_PROBE` is available only on C86 left 64 kHz sampled-probe
apps with INFO capability `0x1000`. Request `[0]` arms one 500 ms window
and returns a nonzero u32 run ID. Start conditions, ACK drain (up to 50 ms),
vendor command exclusion and USB-session cancellation are the same as for
`0x45`. The broad CPU profiler is absent from these builds.

After at least 600 ms from the ACK, request `[1,page]`, pages 0..2. Page 0
contains ten u32 in this order: run ID, elapsed cycles, CPU Hz, target
cycles, accepted samples, discarded samples, report publications,
USB completions, ADC sequence delta, reserved zero. Pages 1..2 use the
44-byte row encoding from `0x45` with stage IDs 0 total body, 1 pre-copy
checks, 2 DMA copy, 3 post-copy validation/staging, 4 Hall average,
5 publication/remainder. All multi-byte fields are little endian.

Each accepted sample contributes to all six rows and the totals of stages
1..5 add to stage 0. Maxima are per row and are not additive. The body
excludes exception entry/exit and the sample recorder. These are sparse
execution spans, not an absolute CPU occupancy or input latency measure.

All update commands retain the C7 framing, CRC, nonzero transaction and zero padding. Left-local opcodes0x30–0x36; right-forwarded opcodes0x50–0x56. Reply uses original opcode|0x80 and original transaction. The left translates only the opcode and CRC over the RS-485 link. One request is outstanding; no concurrent app commands while the updater owns the vendor channel.

| Local op | Request | Success reply / effect |
|---|---|---|
|0x30 INFO|empty|48bytes detailed below|
|0x31 ENTER|ASCII `UPD3`|empty; app ACK completes before backup-cookie reset into boot; boot is no-op|
|0x32 BEGIN|UID12, hw u8=86, role u8, reserved u16=0, nonzero session u32, padded size u32, CRC32 u32, version u32|empty; boot validates target/range then erases app sectors2 then1|
|0x33 WRITE|session u32, offset u32, data32|empty; sequential32byte flashwords; identical last-word retry is accepted without reprogramming|
|0x34 COMMIT|session u32|empty; validates entire CRC/vectors/identity, then writes final metadata once|
|0x35 RUN|ASCII `UPD3`|empty; boot ACK completes then resets into valid app; app resumes maintenance scanning/link without enabling output|
|0x36 READ|app-relative offset u32, count u8=1..48|requested bytes; boot only, bounded application image range|

INFO layout: bytes0..3 protocol1/hardware86/role0left1right/mode0boot1app;
u16 FlashKiB at4; u16 flags at6(bit0 compatibleboot,bit1 validcommittedapp);
u32 appbase at8,maxbytes12,committedsize16,committedCRC20,committedversion24,
activesession28,nextoffset32; UID12 at36. Version numbers are opaque image-generation
identifiers: current app0x0003000A has package display version `0.3.10`. Permanent
boot compatibility identity remains0x00030000; use the manifest display version,
not inferred bytewise semantic-version fields.

Permanent boot:0x08000000..0x0801FFFF. App:0x08020000..0x0805FFDF, maximum262112bytes,32byte padded. Metadata:0x0805FFE0..0x0805FFFF. Settings6/7 at0x080C0000..0x080FFFFF are never erased/programmed by the updater; sectors3–5 are unused. No boot update, arbitrary address, option-byte change, A/B rollback or signing is provided.

At image+0x400, identity32bytes = ASCII C86FW003(app)/C86BL003(boot), hw86 u8, role u8, layout1 u16, version u32, appbase u32, maxbytes u32, magic0x33444946 u32, inverse magic u32. Factory metadata32bytes = magic0x33505546,size,CRC32,version,target(86|role<<8|layout1<<16),appbase,CRC32(first24bytes),inverse magic; all u32. Validation rejects unreadable ECC data and bad vectors/identity before execution. Initial factoryHEX includes the boot, app and metadata.

App mode accepts only updater INFO/ENTER/RUN. Writes exist only in the C86 boot build and require boot VTOR/main context/D-cache off. Reading right INFO pauses left acquisition/output and starts maintenance relay; explicit local RUN resumes it. USB disconnect or failed ACK cancels pending reset actions; no automatic HID enable or settings SAVE. Left boot powers the satellite only while servicing relay, and turns it off before its own blocking flash write. CC protection remains active during right flash operations.

Host BEGIN timeout15s; other commands3s. Relay timeout14s for BEGIN and2.5s otherwise. Replies must complete within3s or reset/resume actions are discarded. Firmware accepts identical BEGIN/session/manifest and identical last WRITE retries, but shipped host tools do not automatically repeat destructive commands. A prior session requires an explicit full power cycle before a new host attempt. Invalid/incomplete apps remain in recovery boot; this design still needs physical power-loss testing.


## C86 v0.3.2 link timing diagnostics

`0x41 LINK_PERFORMANCE` is read-only, empty request; reply52bytes holds13 little-endian u32: micros,polls,accepted,timeouts,sequence_errors,late_replies,uart_errors,tx_busy,tx_fail,crc_errors,framing_errors,rtt_min_us,rtt_max_us. Counters are local to the master, reset at link initialization and may wrap; RTT extrema are lifetime values. They identify transport failures, not ADC noise or Windows receive rate.
