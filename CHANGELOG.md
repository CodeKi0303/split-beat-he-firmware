# Split-Beat HE Firmware 변경 기록

새 버전이 위에 있습니다. 0.3.x까지는 태그를 `c86-vX.Y.Z`로 붙였고, 다음 릴리스(0.4.0)부터 `vX.Y.Z`를 씁니다. 하드웨어 리비전은 릴리스 파일 이름(`c86-left-app.fw.json` 등)에 표시합니다. 규칙은 [VERSIONING.md](VERSIONING.md)에 있습니다.

릴리스 없이 main에만 들어간 버전(예: 0.3.44)은 다음 릴리스에 포함됐습니다.

## v0.6.0 — 2026-11-28

- C86에서 Hall 키마다 **WISH 방식 · Owlab Ti HE 3.4 mm** 거리 곡선을 선택할 수 있습니다. 무입력과 바닥의 ADC 보정값 사이를 자석 모델로 환산해 작동점·해제점·RT와 보고 깊이에 적용합니다. Studio의 센서 진단 그래프에서 기존 참조 곡선과 비교할 수 있습니다.
- 모델은 초기 120 G, 바닥 750 G의 공칭 사양을 사용합니다. 실제 이동거리 오차는 C86에서 두께를 아는 심으로 확인해야 합니다. [모델과 호환 조건](PROTOCOL.md#v060-wish-distance-model)
- C86 한 세트에서 좌우 앱을 USB로 업데이트하고 전체 이미지를 다시 읽어 대조했습니다. 설치 전후 보정·매핑·RT 설정이 일치했고, 키 출력과 좌우 통신이 정상으로 돌아왔습니다.

### v0.6.0에 함께 배포한 v0.5.0 변경

- Hall 키에서 RT를 끄면 고정 해제점을 키별·프로필별로 설정할 수 있습니다. 키는 작동점에 도달할 때 눌리고 해제점까지 올라왔을 때 풀립니다. 두 지점 사이에서 조금 움직여도 눌림이 유지됩니다.
- 초기값과 이전 설정은 작동점 하나로 눌림과 해제를 판단합니다. 고정 해제점은 RT가 켜져 있는 동안 보관됩니다. 장치 명령과 저장 형식은 [PROTOCOL.md](PROTOCOL.md#v050-fixed-release-point)에 있습니다.

## v0.4.0 — 2026-11-24

#### Four on-device profiles

- 작동점·눌림/복귀 RT·상/하단 데드존·RT/재입력 옵션과 28개 키 매핑을 프로필 4개에 따로 둡니다. 보정값, 노이즈 데드밴드, 거리 곡선은 모든 프로필이 함께 씁니다.
- 새 명령 `0x4D`로 프로필 상태·이름을 읽고, 전환·복사해서 만들기·삭제·이름 바꾸기를 합니다. 지원 기능 비트는 `0x1000000`입니다.
- 설정 저장 형식이 80으로 바뀝니다(2 KB 기록). 업데이트해도 기존 설정은 프로필 1로 그대로 불러옵니다. 첫 저장은 다른 섹터에 쓰므로 0.3.x 기록이 한동안 남지만, 0.3.49 이하로 되돌리면 그 사이 저장한 내용은 읽지 못합니다.
- 태그 형식이 `c86-vX.Y.Z`에서 `vX.Y.Z`로 바뀌는 첫 릴리스입니다.

시뮬레이터와 호스트 테스트, ARM 빌드로 확인했습니다. 실제 키보드에서는 아직 시험하지 않았습니다.

## c86-v0.3.49 — 2026-11-21

#### Pointer acceleration for rollers and knobs

Rollers and knobs bound to pointer movement can now accelerate with rotation speed (capability `0x800000`). Turn slowly and each detent moves the set number of pixels; turn fast and each detent moves more. Studio v1.0.18 sets it from the key mapping screen.

- Each detent is timed against the previous detent in the same direction. At 5 detents/s or slower the gain is 1. It then rises linearly to the chosen maximum (×2 to ×16) at 40 detents/s.
- The first detent after a pause of 0.2 s or more counts as slow, and fractions of a pixel carry into later reports.
- With acceleration off, movement is identical to 0.3.48. Hall keys and MX switches are unchanged.

#### Compatibility

- Saved settings are kept. The acceleration strength uses the binding's spare byte, so the settings format is unchanged. A saved profile with acceleration on cannot be loaded by 0.3.48 or earlier, which fall back to an older saved generation or defaults.
- Studio v1.0.17 and earlier do not show or keep the setting.

#### Files and validation

Install both `.fw.json` assets (left and right) over USB. The application ZIP includes source/hash metadata; the first-flash ZIP has SWD factory HEX/boot files for new boards.

Full offline firmware/host/Studio checks passed, including all ARM image builds. New firmware tests cover slow, paused, fast and mid-speed detents, fractional carry and rejection of invalid settings. Python/JS codec parity and Studio browser tests with a simulated keyboard also passed. This build has not been installed on a keyboard before publication, and the 5–40 detents/s range has not been tuned on hardware.

## c86-v0.3.48 — 2026-11-21

#### Move the mouse pointer from keys and rollers

New binding kind 7 moves the mouse pointer left/right or up/down (capability `0x400000`). Studio v1.0.16 sets it up from the key mapping screen.

- **Hall keys:** the pointer moves while the key is pressed and goes faster the deeper you press. The set speed is px/s at full depth.
- **MX switches and knob clicks:** they start at 30% of the set speed and reach full speed after 0.35 s.
- **Rollers and knobs:** each detent moves the set number of pixels.

Motion rides in the X/Y fields the mouse report always declared, so the USB descriptor is unchanged and Windows does not re-enumerate the keyboard. Motion is sent every 1 ms. Fractions of a pixel are carried, larger moves are split across reports, and turning output off stops everything. Pointer bindings never produce keyboard keys or retrigger repeats.

#### Compatibility

- Saved settings are kept. A saved profile containing a pointer binding cannot be loaded by firmware 0.3.47 or earlier; those versions fall back to an older saved generation or defaults.
- Studio v1.0.15 and earlier, and the MCP plugin v0.1.6, do not know binding kind 7.

#### Files and validation

Install both `.fw.json` assets (left and right) over USB. The application ZIP includes source/hash metadata; the first-flash ZIP has SWD factory HEX/boot files for new boards.

Full offline firmware/host/Studio checks passed. That covers all ARM image builds and new firmware tests for depth-scaled speed, report splitting, release stop, stall cap, MX ramp, encoder detents and output-off reset. It also covers a USB test of the X/Y bytes, Python/JS codec parity, and Studio browser tests with a simulated keyboard. This build has not been installed on a keyboard before publication, and pointer feel has not been tuned on hardware.

## c86-v0.3.47 — 2026-11-20

#### Decision values for right-half keys

`ADC_COMPARE` (`0x48`) now accepts all 14 Hall keys. Before 0.3.47 it covered the left half only (0–6).

For a right key (7–13) it returns the code the right half sent, which is already averaged there, in both the latest and average fields. It also returns the left engine's decision code after the noise deadband. Studio v1.0.14 uses this to plot every key's raw, averaged and decision values on its curve graph.

Nothing else changed from 0.3.46: input processing, settings format 79, the custom curve and the 8-scan default average are the same.

#### Files and validation

Install both `.fw.json` assets (left and right) over USB. The application ZIP includes source/hash metadata; the first-flash ZIP has SWD factory HEX/boot files for new boards.

Full offline firmware/host/Studio checks passed, including all ARM image builds and Studio browser tests that use a simulated keyboard to show the right-key values. This build has not been installed on a keyboard before publication.

## c86-v0.3.46 — 2026-11-20

#### Steadier Hall input and a drawable distance curve

- **8-scan moving average by default.** Both halves now boot with a common 8-scan moving average (window 8) instead of the latest scan only. At 96 kHz the window spans about 83 µs. Studio's ADC settings can still change it until the next power-up.
- **Left F / right J sampled twice per scan.** ADC2 key 0 was the only Hall key converted once per scan, so a single disturbed conversion went straight into the RT decision. Idle telemetry on the test keyboard showed brief phantom F presses (40–82 µs) from single-sample spikes. ADC2 now runs six ranks and averages that key's two conversions like every other key. The scan period is unchanged.
- **Custom curve (curve 2).** One user-drawn ADC-to-travel table shared by every key that selects it. It is 15 non-decreasing points on a fixed grid with fixed endpoints; drawing it in Studio's 센서 진단/설정 tab writes it with the new command `0x4C` (capability `0x200000`). Writes are RAM-only like other settings; **키보드에 저장** keeps them.

#### Compatibility

- Saved settings are kept. The journal now writes format 79 (format 78 plus the curve table); format 78/77 records load with a straight-line custom table.
- Downgrading to 0.3.45 or earlier ignores format-79 records and starts from an older saved generation or defaults. Switch every key back to curve 0 or 1 and save before downgrading if needed.
- Studio v1.0.13 is needed to read a keyboard that has a key on the custom curve; Studio v1.0.12 and the MCP plugin v0.1.5 accept only curves 0 and 1. JSON profiles do not include the custom table.
- Also includes 0.3.44 input telemetry and 0.3.45 right-half recovery.

#### Files and validation

Install both `.fw.json` assets (left and right) over USB. The application ZIP includes source/hash metadata; the first-flash ZIP has SWD factory HEX/boot files for new boards.

Full offline firmware/host/Studio checks passed, including all ARM image builds, settings migration 77/78 → 79, exhaustive C/Python/JS curve parity (98,304 samples, including custom tables) and Studio browser tests with a simulated keyboard. On hardware, the F/J pairing was installed on the test keyboard and the user reported much steadier input with the moving average. The 8-scan default and custom curve build itself has not been installed on a keyboard before publication, and typing behavior with a custom curve has not been measured.

## c86-v0.3.45 — 2026-11-20

#### Right half comes back without a USB replug

After a PC power cycle the right half could stay missing until the USB cable was replugged. 0.3.45 fixes two causes on the left master:

- **Split link stopped by its own abort.** When the left removed right-half power while a link request was being sent, the deliberate transmit abort was taken for a fatal DMA error and the link stopped until the keyboard rebooted. The abort is no longer treated as an error, and a link that a real DMA error stops now restarts by itself after 10 ms.
- **CC faults latched until replug.** Any FUSB303B fault (I2C, timeout, status, readback, load switch) blocked right-half power until the PC un-configured USB, which a running OS never does. The supply now stays blocked for 1 s and the CC controller is then reinitialized. Load-switch faults are retried at most three times per USB connection and then latch as before.

Scheduler diagnostics (0x43) keep their layout. The `cc_error` word now also carries the last CC fault, CC retries and link restarts since boot; the host and Studio decoders show them as `cc_last_error`, `cc_retries` and `link_restarts`.

#### Also included: 0.3.44 input telemetry

Opt-in input telemetry (capability `0x100000`, `0x4A` control / `0x4B` read): Hall samples, press decisions and USB report changes with device microsecond time, for aligning with a game's own input log. RAM only, off at boot, no change to input behavior, output or saved settings.

#### Files and validation

Install both `.fw.json` assets (left and right) over USB. The application ZIP has their source/hash metadata; the first-flash ZIP has SWD factory HEX/boot files for new boards. Saved settings, calibration and mappings are kept.

Full offline firmware/host/Studio checks passed, including all ARM image builds and new host tests for the CC retry/limit, the abort flag handling and the diagnostic packing. Not yet verified on hardware: the recovery has not been exercised on a real PC power cycle, and this release has not been installed on a keyboard before publication.

## c86-v0.3.43 — 2026-11-18

#### 0.001 mm Hall settings

Hall travel is now stored in 1/10000 of the switch stroke (0..10000, capability `0x80000`) instead of 1/1000. At a 3.4 mm stroke one unit is about 0.00034 mm, so actuation, press/release RT and top/bottom deadzones can be entered in 0.001 mm steps.

- Saved settings are kept. Records written by 0.3.42 and earlier are converted (x10) at boot, so every threshold keeps the same depth; the next save writes the new format.
- Downgrading to 0.3.42 or earlier ignores settings saved in the new format and starts from the previous saved generation or defaults.
- The setting step is not the sensor resolution. Decisions still move with the averaged ADC sample, roughly 0.008 mm per ADC code with the current C86 calibration span.
- Packet layouts, raw ADC, calibration endpoints, noise deadband, curves, mappings and the permanent bootloader are unchanged. Studio v1.0.11 and the MCP plugin v0.1.5 use the new unit and still connect to older firmware, rounding mm input to its 1/1000 step.

#### Files and validation

The two `.fw.json` assets are the paired 96 kHz C86 USB application updates; install both. The application ZIP includes their source/hash metadata, and the first-flash ZIP contains verified SWD factory HEX/boot files for new boards.

Full offline firmware/host checks passed, including all ARM image builds, format-77 settings migration, C core / Python / Studio codec parity and old-firmware conversion. Both halves of the test keyboard were updated from 0.3.42 over USB with full app readback; the saved profile (generation 24) read back unchanged, and a short read-only check showed an 8 kHz split link, 96 kHz ADC and about 8 kHz USB completions with no new errors. Typing behavior at 0.001 mm settings has not been measured.

#### 자세히 (이전 README에서 옮김)

**0.001 mm 설정 단위 (v0.3.43)**

v0.3.43부터 Hall 이동량을 총 스트로크의 1/10000 단위(0~10000)로 다룹니다. 3.4 mm 스위치에서 1단위는 약 0.00034 mm이므로 작동점·눌림/복귀 RT·상/하단 데드존을 0.001 mm 간격으로 입력할 수 있습니다. 이전 버전은 1/1000 단위(약 0.0034 mm)였습니다.

- 업데이트해도 저장된 설정은 유지됩니다. 이전 형식의 값은 부팅할 때 10배로 변환되어 같은 깊이를 유지하고, 다음 저장부터 새 형식으로 기록됩니다.
- 이전 펌웨어로 되돌리면 새 형식 저장값을 읽지 못하고 그 전에 저장한 설정(또는 기본값)으로 시작합니다.
- 입력 단위가 센서 정밀도를 뜻하지는 않습니다. 판정은 ADC 1단계(C86 보정 범위 기준 키당 약 0.008 mm) 단위로 움직이므로, 그보다 작은 차이는 같은 동작이 될 수 있습니다.
- Studio와 MCP는 이전 펌웨어에도 연결됩니다. 이때 mm 입력은 총 스트로크의 1/1000 단위로 반올림해 적용합니다.

## c86-v0.3.42 — 2026-11-12

#### Shared initial key layout

The initial 28-input mapping now matches the saved Split-Beat HE layout captured on 2026-11-13. Firmware, Studio and the MCP plugin use the same binding-only baseline in `default-bindings.json`.

- Left Hall: Tab, A, S, D, F, Left Shift, Space.
- Right Hall: Enter, semicolon, L, K, J, Right Shift, Right Alt.
- Digital/click inputs and all eight wheel/knob directions also match the baseline.
- Valid saved settings always take precedence. Updating does not replace existing mappings, Hall calibration or RT settings. New boards still need their own Hall calibration.

This release includes the v0.3.41 deadzone endpoint reporting and the previously installed ADC improvements. Reported travel is 0 inside the top deadzone and maximum stroke inside the bottom deadzone; internal RT decisions and raw diagnostics are preserved.

#### Files and validation

The two `.fw.json` assets are the paired 96 kHz C86 USB application updates. The application ZIP includes their source/hash metadata. The separately named first-flash ZIP contains verified SWD factory HEX/boot files for new C86 boards; ordinary updates must not overwrite the permanent bootloader or settings sectors.

Full offline firmware/host tests, serialized default-mapping parity, saved-settings retention and firmware image checks passed. v0.3.41 was installed and measured on both boards; v0.3.42 changes the initial mapping/version and host guidance and has not been flashed onto the existing test keyboard. No new blank-board physical qualification is claimed.

## c86-v0.3.37-rc.1 — 2026-11-12 (시험판)

C86 v0.3.37 candidate: optional retrigger before release, OFF by default.

With RT enabled, a partial upward movement followed by the configured downward RT distance can send a new release/press. Completion-tagged USB phases preserve other held inputs and cancel on real release, output off, stale input, or timeout.

Both left and right apps identify as 0.3.37. Existing ADC/scan/filter choices and calibration/mappings remain unchanged. Requires Studio v1.0.5 or MCP plugin v0.1.3 for the named setting.

Offline full checks and role-specific image verification passed; C/Python/JS parity matched 79,200 steps across144 configurations. This candidate has not been installed or measured on hardware. Synthetic release can break long notes, and some games can miss a short pulse. Auto-fit/timing analysis is restricted in this mode.

Before downgrading to older firmware, turn this option OFF on all keys and save/read back settings. Older firmware rejects the new configuration flag.

This prerelease does not replace stable v0.3.36 in Studio.

## c86-v0.3.36 — 2026-11-09

Both C86 halves start with one scan per Hall key (Raw). Apply runtime scan rate and averaging to both halves together. Adds right-side readback and requires resynchronization after satellite link loss. Both 96 kHz app packages passed offline verification.

## c86-v0.3.35 — 2026-11-09

Fixes a downstream power-controller timeout reproduced when changing the moving-average window in v0.3.34. Filter-only updates keep ADC acquisition running. Scan-rate changes rearm DMA and the timer without rerunning ADC calibration. Both C86 halves passed full image readback, 24 repeat filter changes and all 25 scan-rate/window settings on a physical split keyboard. Saved key settings are preserved.

## c86-v0.3.34 — 2026-11-09

Adds a single Studio action for setting the scan rate and moving-average window on both C86 halves. It applies to the right board first, checks its acknowledgement, then applies locally. Both applications use the same version. Settings are RAM-only. The right-board change and the paired transaction passed offline and simulated checks. Testing with both physical boards connected is pending.

## c86-v0.3.30 — 2026-11-08

Aligns the stable left and right C86 applications at v0.3.30. The compiled applications differ from their previously published counterparts only in embedded version values. Both WebHID packages are included.

## c86-v0.3.23 — 2026-11-08

Previously hardware-verified C86 USB apps. Left v0.3.23, right v0.3.20. Requires the C86 USB updater bootloader on both halves. See the SWD guide for blank boards.
