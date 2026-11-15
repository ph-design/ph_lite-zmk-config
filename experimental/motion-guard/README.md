# Motion Guard / keyboard power preview.9 · 拿起锁定

This is an optional overlay after `../studio-2g4/`, not a replacement for the default keyboard build. It combines the tested preview.4 policy / encrypted telemetry with additive, validated settings. Preview.7 adds hardware-assisted tap detection, diagnostics and corrected LIS2DH sample conversion.

## Fixed baselines and application order

- Core: `ph-design/zmks` `611132e871678a0aed9c1fd9258ec39033046b8e`.
- Configuration: `ph-design/ph_lite-zmk-config` `08b45141e7d0f340d79e5ff8b20505667c46a94e`.
- Messages: `ph-design/zmk-studio-messages` `dd1c1a8ab550d6d6045f6ea6556538d429d8d242` (the module selected by the Zephyr workspace).
- Zephyr 3.5 / Zephyr SDK 0.16.8 for EVO. Hub uses its separate NCS environment.

Use isolated core/config/messages checkouts. Apply `studio-2g4/zmk.patch` to core and `config.patch` to config. Copy the source/header additions described in that README, including `pairing_subsystem.c` and `pairing.proto`. Do not overwrite the final core with old snapshots after applying this overlay. The config CMake must use `${CMAKE_CURRENT_SOURCE_DIR}/src/studio-radio.c` rather than a previous machine's absolute path.

```sh
python3 /path/to/experimental/motion-guard/apply.py /path/to/zmk /path/to/zmk-studio-messages --check
python3 /path/to/experimental/motion-guard/apply.py /path/to/zmk /path/to/zmk-studio-messages
sh /path/to/experimental/motion-guard/test.sh /path/to/zmk
west build -s /path/to/zmk/app -d build-evo-guard -b ph_lite -- \
  -DSHIELD=PH60-SC_V2_EVO -DSNIPPET=studio-rpc-usb-uart \
  -DZMK_CONFIG=/path/to/config/config -DZMK_EXTRA_MODULES=/path/to/config \
  -DEXTRA_CONF_FILE=/path/to/experimental/motion-guard/evo.conf
```

The messages argument must be the module actually used by the build; use a fresh build directory so nanopb regenerates the extended messages. `apply.py --check` verifies the original motion RPC/protocol hashes and checks the integration patch before any write. It refuses an existing overlay. `integration.patch` contains modifications to existing core files; adjacent source files supply the new policy, telemetry header and RPC/protocol replacements.

The keymap is taken from the user's configuration. No private pairing record, device dump, personal layout or prebuilt image is part of this overlay. The compatibility AES key in `evo.conf` is the existing public factory configuration; independent pairing is still recommended.

## Behavior

The guard runs before position events. Coherent sideways/inverted poses take the fast lock path after sample confirmation; ambiguous sustained departure uses the configured verification period. Normal READY/VERIFY key events stay on the direct path. Only new presses during stable placement are buffered, then replayed in order at a bounded rate; keys held in the bag cannot become fresh landing presses. A brief flat sample after continuous movement cannot unlock: the full per-axis window must remain stable. Sensor reads stay on a sampler thread; no I2C or flash write is introduced on a key event.

Idle power reduction remains enabled. Default automatic System OFF is disabled so the IMU and input path remain responsive. Preview.6 exposes independent standby/light timers and optional deep sleep; manual power-off remains available. A stale/unavailable sensor locks input until recovery or explicit disabling through Studio.

The encrypted existing keepalive adds `[FE,battery,A1,state]` (1 READY / 2 LOCKED), plus a deferred report after a state transition. Lost-link retry probes continue at 1 second so a locked keyboard can reconnect without a physical key event. Hub 0.6.7 aggregates the two peers independently.

## Settings and wire compatibility

| Field | Default | Range |
| --- | ---: | ---: |
| carried_settle_ms | 180 ms | 100–600 ms |
| brief_settle_ms | 60 ms | 40–200 ms |
| stable_span_mg | 60 mg | 20–100 mg |
| departure_mg | 200 mg | 120–350 mg |
| verify_ms | 30 ms | 20–60 ms |

Long confirmation must be at least brief confirmation. Stricter stability can delay unlocking on a vibrating desk; lower departure/verification values can react to typing impulses. Defaults retain the previous tested behavior. Actual timing is quantized by the roughly 10 ms sample cadence.

`Capabilities.guard_version` is uint32 field 8 (version 1). `CarryConfig` adds uint32 fields 4–8 in table order. Existing fields keep their meanings. Old clients sending no extension retain the current guard settings; partial/invalid new settings are rejected as a whole. ZMKs requires explicit version support and complete valid values before showing writable parameters. Applying is separate from saving: standard motion save writes a validated/versioned `motion_guard/v1` record, and motion settings reset clears it too. Missing or invalid records retain defaults.

## Validation

The default 21 policy scenarios, 10000 bag cycles and 40000 typing-vibration taps pass with address/undefined sanitizers. Additional tests cover bounds, cross-field constraints and all five parameter effects. Host harnesses execute the real reset behavior and heartbeat sender. Fresh EVO compilation succeeds. Hardware flash/reboot parameter readback and changed-parameter input behavior remain to be tested by the user; no preview.6 device flash was performed. See sibling Hub VALIDATION.md for receiver hardware evidence and its limits.

## Runtime power settings (preview.6)

The Device page in ZMKs exposes automatic standby (default 120 s), RGB suppression (30 s) and independent backlight suppression (30 s, shown only if built). Zero disables an individual timer. Timers use whole seconds up to 24 hours. The USB policy defaults to bypassing timed standby/light suppression. Lock-triggered light suppression is independent and defaults on. Manual light on/off, color and effect settings are never overwritten by suppression.

Deep sleep is a separate opt-in timer, disabled by default. When enabled it must be at least 60 s and no earlier than any enabled standby/light timeout. It enters System OFF only on battery, with no held key, no active lift lock and Studio locked. It stops the IMU and connection; a key wakes the keyboard, and the wake key may not be emitted. This option deliberately trades first-key continuity for lower power. Defaults preserve the user's preferred fast response.

Activity is based on input / meaningful action, not management reads or keepalives. Held physical keys keep the clock current, so holding a game key does not sleep or extinguish lights. Release restarts the interval. Guard unlock records activity. Applying power settings also restarts the interval to avoid an immediate transition while editing. All light I/O is queued separately from input; the old activity listeners no longer update hardware directly.

Wire extension: `Capabilities.power_version` field 9 is 1; `power_lights` field 10 is a bitmask (RGB=1, backlight=2). Motion requests 12/13 get/set `PowerConfig`, responses 12/13 return config/acceptance. PowerConfig uint32 fields 1–4 are idle/rgb/backlight/deep milliseconds; bool fields 5/6 are on_usb/off_when_locked. Existing motion and guard fields are unchanged. Settings save/reset includes an independent validated versioned `power/v1` record. No input or periodic idle check writes flash.

`apply.py` also checks/applies `power-lights.patch`, adds `power.c` to the build, and installs the runtime activity implementation. `test.sh` covers independent timers, boundaries, held input, USB/Studio/lock gating, and runs actual `power.c` against mocked clock/workqueue/devices, including validated save/load/reset. Physical power consumption, deep-sleep wake behavior and persistence across a real reboot remain user tests; host harnesses are not physical measurements.

Before System OFF the sampler is stopped and an in-flight sensor bus operation is allowed to complete under a sampler-only mutex. Pending policy/replay work is cancelled. A failed device suspend resumes sampling. Standby does not stop the sampler, and the mutex is never taken by key processing.

## Tap detection and driver corrections (preview.8)

LIS2DH normal 10-bit samples previously used an approximate full-scale conversion: nominal 1 g decoded to approximately 0.976 g. Conversion now uses the datasheet nominal sensitivity for each range and 8/10/12-bit mode, explicit signed little-endian decoding and 64-bit intermediate arithmetic. BDU prevents mixed high/low bytes. Tests execute the actual patched fetch/conversion/channel getters. Register order is X/Y/Z; physical PCB-to-case axis orientation has not been measured, so no speculative X/Y swap is applied. Studio shows sensor-axis values in mg for this measurement. Real sensor tolerances still apply.

Enabled tapping selects 400 Hz hardware ODR; disabling restores 100 Hz. The guard consumes frames at its existing approximately 100 Hz cadence. I2C remains on the sampler thread. The hardware click engine always selects double-click detection, including no-action test mode. Default mode is one case-wide gesture, all sensor axes, 120 ms typing quiet interval. Left/right-by-polarity remains an explicit experimental option and is not reliable physical localization. Existing right-side bindings are preserved while hidden in unified mode.

Preview.8 disables all single-tap actions, including previously saved legacy bindings. It does not synthesize double clicks from software single events and does not implement triple taps. Existing single binding wire/storage fields remain for compatibility but are never invoked. Capabilities.tap_version is 2; configuration and diagnostic layouts are unchanged from version 1. Physical key processing gains no new delay. Tap dispatch waits 12 ms to allow a coincident physical key to veto it, and rejects held/recent keys, lift-lock transitions, stale configurations and queue overflow. Queued events carry a guard generation so pre-lock events cannot execute after placement.

Studio provides a 60-second no-action test, GPIO/recognition/gesture/action/suppression counters, actual register snapshot and a bounded 32-frame waveform with actual timestamps. Test events remain non-executing even if processed after test expiry. Snapshot reads do not reread the self-clearing CLICK_SRC register. Test sampling is best-effort and is not an oscilloscope or guaranteed 400 Hz host recording.

Wire extension: Capabilities uint32 tap_version=11 is 2 (version 1 is preview.7). TapConfig fields 11/12/13 are axes_mask (1..7), split_sides (bool), typing_quiet_ms (0..1000). Complete extensions are validated transactionally; old clients preserve current extra options. Requests 14/15/16 get diagnostics, set test (bool), get capture; responses 14/15/16 return diagnostics, acceptance, capture. Capture bytes are at most 256 bytes: 32 little-endian frames of uint16 milliseconds and int16 X/Y/Z mg. Options persist independently as tap/v1 only on explicit Save; reset includes this record. Existing binding fields remain unchanged. The messages module must provide nanopb.proto, as the pinned module does.

Validation: host policy and real-controller tests, actual driver functions across modes/ranges and fresh EVO compilation pass. Physical axis orientation, click recognition rate, false triggers, power use and real reboot persistence need user testing. No preview.8 keyboard flash was performed. The nominal scale correction slightly changes motion thresholds in physical units; repeat carrying/placement and typing checks after flashing.

References: [LIS2DH12 datasheet](https://www.st.com/resource/en/datasheet/lis2dh12.pdf), [ST AN5005](https://www.st.com/resource/en/application_note/an5005-lis2dh12-ultralowpower-highperformance-3axis-nano-accelerometer-with-digital-output-stmicroelectronics.pdf).

## Studio layout (preview.8)

Double-tap bindings, the relative threshold value, double-tap window and layer selection appear first. A battery-life notice explains the small additional sensor consumption. Advanced axes/direction/typing suppression/timing controls and hardware diagnostics are collapsed at the page bottom. Diagnostics mount and poll only while expanded; closing the section stops its own no-action test. Threshold units and defaults are unchanged. Version 1/legacy firmware shows an upgrade notice; applying a tap setting clears old single actions, with explicit Save required for persistence. New firmware ignores stored singles even before such a save.

Preview.8 kept the original classifier; preview.9 below replaces its single-frame pose path. Hardware behavior and battery draw need user verification.

## Sensorless boards and coherent pose policy (preview.9)

Supported build matrix: PH60-SC_V2_EVO (IMU), PH60-SC_V2 and PH60-Multi_V2 (no IMU), all on ph_lite. Power management now builds independently of CONFIG_ZMK_MOTION. Sensorless builds use no_imu.c to pass position events directly, never start a sampler or startup lock, and report no motion capabilities. A separate secured power-only RPC implementation uses the existing motion wire subsystem for capabilities, get/set power, explicit save and reset. This keeps standby, independent lights and opt-in sleep configurable without inventing an IMU. No sensor handlers or chip driver references are linked on those boards. Studio hides Motion when all motion features are absent while keeping the Device power editor and save path.

The guard now uses a three-frame per-axis median for pose/departure evidence, rejecting isolated 10 ms impulses that used to take the immediate sideways/inverted path. Fast pose locking additionally requires a gravity-sized vector (700..1300 mg). At the normal approximately 100 Hz policy cadence, a sustained sideways pose is confirmed after two changed samples (approximately 20 ms including initial sample latency). Ambiguous departure still uses the configurable confirmation interval, now with coherent pose data. Ordinary key dispatch is unchanged; typing and held keys do not disable lift detection. A single accelerometer cannot prove motion at the exact instant of pickup; coherent detection introduces this bounded sample-confirmation tradeoff. No guarantee of zero leakage before detection is claimed.

Landing confirmation deliberately uses unfiltered raw per-axis stability over the complete 60/180 ms default window, so filtering cannot make a moving keyboard appear settled. Queuing, replay order, bag-held-key suppression, carry history, stale sensor lock and double-tap gates remain intact. Thresholds and saved settings are unchanged. Physical case impulses that last for several frames can still resemble actual movement and must be tested; this is an experimental mitigation, not measured proof of spacebar immunity.

Validation: fresh source/SDK builds succeed for all three shields. Actual sensorless adapter tests cover immediate forwarding of 128 positions, no lock and unsupported sensor operations. The existing guard suite plus isolated sideways/inverted/overrange impulses, held-key pickup, stale history, 10000 bag cycles and 40000 vibration taps passes. Driver, tap, power, BL and encrypted telemetry regressions pass. No keyboard was flashed. Physical typing/pickup/bag behavior, power draw and persistence across a reboot remain user tests.
