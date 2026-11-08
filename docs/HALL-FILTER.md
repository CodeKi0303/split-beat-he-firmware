# C86 left Hall input filter — v0.3.6 preparation

Update2026-11-07: the left app was subsequently installed with user approval.
Released-key decision noise and runtime counters were measured; see
internal installation measurements. The design discussion
below describes the prepared implementation. Dynamic typing/RT stability and
end-to-end latency still require separate testing.

The released-key measurements on2026-11-07 found left SHIFT/SPACE raw ADC
standard deviations near10/14 counts, versus about1–1.8 on the right. One left
W STATUS travel sample crossed its configured actuation threshold. This noise
predates the Owlab curve; the curve magnifies near-rest ADC variation by about
3.244 relative to the old linear map. `noise_deadband=2` alone does not average
samples and passes larger fluctuations. The raw measurements and limitations
are recorded in `SAVE-RECONNECT-AND-LEFT-NOISE-20261107.json`.

## Prepared change

C86 **left local ADC only** uses a rolling average of8 consecutive complete
scans before the existing ADC deadband, curve, actuation and RT logic. The
separate16k recovery build uses2 samples; C77/C85 and the right satellite keep
their existing path. There is no new user setting or persistent-profile field.
ADC conversion clock/sample time,64k scan target and125us USB interval remain
unchanged. A left app update is sufficient; the existing right app and permanent
bootloader do not need replacing.

The filter runs on each DMA completion, not in the variable-rate main loop.
Each channel has an independent ring and running sum. The first real sample
seeds the window; restart discards old history. Original raw samples remain
available for diagnostics and calibration. Only a separate valid averaged
sample feeds input decisions. The S7 packet is unchanged and decoding clears
the optional internal filter fields. Remote samples cannot enable the local
filter path.

| Acquisition | Window | Linear-phase group delay | Conservative step settling |
|---|---:|---:|---:|
|64kHz normal|8 samples|54.6875us|within125us|
|16kHz recovery|2 samples|31.25us|within125us|

These are mathematical filter delays. Trigger timing also depends on input
slope, threshold, RT state, IRQ scheduling and USB phase; they are not measured
end-to-end latency. An isolated additive spike is reduced to approximately1/8
at64kHz, subject to integer rounding. Low-frequency drift, correlated noise,
and sustained disturbances are not guaranteed to vanish. Host50/100Hz samples
must not be smoothed offline and presented as a test of the64kHz filter.

The averaged output is rounded to an ADC count. No extra physical resolution
is claimed: a0.01mm UI increment remains distinct from actual measurement
accuracy. Live UI distance scales the device's `STATUS.travel`; raw ADC numbers
and diagnostic graphs remain raw, so they may still visibly fluctuate after
the decision path becomes smoother.

## USB-related hypothesis

The left MCU alone runs the USB3300 ULPI interface. The frozen C86 pin manifest
places SHIFT on PA4/pin28, ULPI_CLK on PA5/pin29, and SPACE on PA6/pin30. That
is a reason to investigate local high-frequency coupling; it does not prove
that USB power, grounding, the clock, or a specific route caused the measured
noise. The two noisy channels were strongly anticorrelated, which also makes
a simple common power shift an incomplete explanation.

ST's [AN5354, section6](https://www.st.com/resource/en/application_note/an5354-getting-started-with-the-stm32h7-series-mcu-16bit-adc-stmicroelectronics.pdf)
recommends keeping digital activity and return paths away from sensitive analog
signals, a solid ground reference, and appropriate ADC input/voltage-reference
design. This is general design guidance, not a diagnosis of this assembled PCB.

## Qualification and installation boundary

Offline tests must exercise the actual filter/publication code, startup and
restart, positive and negative spikes, alternating samples, sustained press and
release/RT, independent channels, invalid ADC bounds, raw/filtered separation,
remote/legacy bypass, and existing DMA ownership/overrun guards. GUI tests must
show device travel for live mm while retaining raw diagnostics.

After an explicitly authorized left app update, repeat the released-key
measurement and deliberate press/release tests, check the saved profile is
unchanged, and remeasure ADC/USB/link counter deltas and maximum IRQ time. The
candidate is not hardware-qualified until these checks pass. Preparing the
package does not flash it or enable input.
