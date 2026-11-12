# Owlab Ti HE travel curve — v0.3.5 preparation

The user supplied an Owlab Ti HE specification showing 3.4 mm total travel on
2026-11-07. This is the selected nominal stroke, not proof that every installed
switch travels exactly 3.4 mm. Keycap height is not used.

## Reference and proportional scaling

Curve 1 is an **approximate OW Ti reference curve**, based on the numeric
`Tab60 HE` / `OW` / `OW Ti` rule in the public NeonHE frontend:
<https://neonhe.com/assets/index-C2gKFU8p.js>.
Its source distance nodes end at 3.3 mm. We retain their normalized shape and
scale the output to the user's selected total stroke, initially 3.4 mm.
This is not a measured C86 transfer function or proof of NeonHE firmware's
internal ADC conversion. In particular, we interpret its `pcnt` nodes as
normalized signal fractions for our own mapping; that interpretation is not a
verified description of NeonHE firmware.

| Normalized signal, 0–10000 | Source distance units, 0–33000 |
| ---: | ---: |
| 0 | 0 |
| 467 | 5000 |
| 1211 | 10000 |
| 2244 | 15000 |
| 3667 | 20000 |
| 5644 | 25000 |
| 8500 | 30000 |
| 10000 | 33000 |

Each key keeps its own measured released and fully pressed ADC endpoints.
After polarity-aware normalization and clamping, monotonic piecewise linear
interpolation gives normalized estimated travel. AP, press/release RT and both
deadzones use that corrected travel in firmware. Both motion directions use
the same curve, so changing direction does not cause a mapping discontinuity.

From firmware0.3.41, reported travel is0 inside the top deadzone and1000 inside
the bottom deadzone, including both boundaries. Values between the bands are
unchanged. Internal RT calculations and raw-derived curve estimates keep the
unclamped curve coordinate, preserving press/release/retrigger behavior.

The display uses `estimated_mm = reported_travel / 1000 * total_travel_mm`.
For another switch with stroke T, the same point on the curve is therefore
`Owlab_estimated_mm * T / 3.4`. This is the user's requested approximation:
changing stroke does not characterize a different magnet or sensor geometry.
Users should still capture released/full-press endpoints for each key.

All firmware/host integer curve paths use this rounding contract:

1. Clamp below released and above full press after correcting ADC polarity.
2. `signal = floor((distance * 10000 + floor(span / 2)) / span)`.
3. In segment `[x0, x1]`, `y = y0 + floor(((signal-x0)*(y1-y0) + floor((x1-x0)/2))/(x1-x0))`.
4. `travel = floor((y * 1000 + 16500) / 33000)`.

Curve 0 preserves the previous linear firmware computation exactly. Existing
profiles and Flash records with zero reserved bytes remain curve 0. Selecting
curve 1 changes the meaning of the normalized RT distances; existing numerical
settings are preserved and can then be adjusted in mm. No automatic threshold
retuning or calibration endpoint replacement takes place.

## Measurement limits and activation

The earlier caliper fixture coordinates are explicitly **not verified stem
displacement** and are not used as physical-mm curve knots. The 3.4 mm endpoint
specification alone does not establish the middle of the curve. A 0.01 mm UI
increment is not a physical accuracy claim.

The key record adds `curve_id` at bytes 18–19 with capability `0x200`. Firmware
v0.3.5 is required to apply curve 1 to real input processing. A newer UI must not
pretend that old firmware's RT uses the nonlinear curve. Total-travel metadata
alone can be changed on old firmware, but its processing remains linear.

Actual firmware installation, curve application, Flash persistence and output
enable are separate operations. Preparing this change does not install it.
The right board supplies raw samples; the left master evaluates all 14 Hall
keys. A left application update is sufficient for this feature with the
existing compatible right-side link firmware.
