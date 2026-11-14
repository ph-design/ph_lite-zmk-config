# EVO Motion Guard preview.5 · 拿起锁定

This is an optional overlay after `../studio-2g4/`, not a replacement for the default keyboard build. It combines the tested preview.4 policy / encrypted telemetry with additive, validated settings. No tap-detection algorithm, axis selection, trigger callback or ODR change is included in preview.5.

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

The guard runs before position events. Clear sideways/inverted poses lock immediately; ambiguous sustained departure uses the configured verification period. Normal READY/VERIFY key events stay on the direct path. Only new presses during stable placement are buffered, then replayed in order at a bounded rate; keys held in the bag cannot become fresh landing presses. A brief flat sample after continuous movement cannot unlock: the full per-axis window must remain stable. Sensor reads stay on a sampler thread; no I2C or flash write is introduced on a key event.

Idle power reduction remains enabled. With a healthy enabled guard, automatic System OFF is suspended so the sensor can continue confirming pickup/placement; manual power-off remains available. Disabling the guard retains normal sleep behavior. A stale/unavailable sensor locks input until recovery or explicit disabling through Studio.

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

The default 21 policy scenarios, 10000 bag cycles and 40000 typing-vibration taps pass with address/undefined sanitizers. Additional tests cover bounds, cross-field constraints and all five parameter effects. Host harnesses execute the real reset behavior and heartbeat sender. Fresh EVO compilation succeeds. Hardware flash/reboot parameter readback and changed-parameter input behavior remain to be tested by the user; no preview.5 device flash was performed. See sibling Hub VALIDATION.md for receiver hardware evidence and its limits.
