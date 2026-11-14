# Hardware validation — 2026-11-14

Setup: Pro Micro / SuperMini nRF52840 Hub, two battery-powered PH60-SC_V2_EVO keyboards, Inphic IN10 BLE HID mouse, PC Bluetooth disabled.

- Both keyboards and the mouse were confirmed to input correctly at the same time. The final mouse test did not power-cycle the mouse. A stuck Shift was released by physically pressing/releasing both Shift keys; neither keyboard was rebooted.
- Two wireless Studio slots returned different device identities, the 2.4G endpoint and USB unattached. Changing slots through AT while management was open was rejected. The older single-peer pairing was preserved.
- After a read-only five-layer backup and explicit user authorization, the second keyboard's existing unsaved configuration was saved. It returned success in about 844ms; readback matched and dirty was false. The first keyboard was not written. This save ran with Hub 0.6.1; later updates changed only the Hub, and no flash save was repeated on 0.6.5.
- Forgetting the mouse cleared stale scan candidates and preserved both keyboard pairings. Scanning and secure pairing completed again. The final firmware explicitly sets Report mode and sends Exit Suspend; the mouse then produced input without a mouse power cycle.
- A diagnostic build captured the unpatched ESB WAIT_MPSL assertion after a 30-second scan expired. After limiting PRX extensions to 32, the same scan-expiry/idle sequence completed without a retained fault.
- Final 0.6.5 status: 187 decoded BLE input reports, zero parse failures; both keyboard counters nonzero, zero decrypt failures and queue drops; HUB_FAULT=0 and safe mode off. Then 20 management-port open/close cycles and 60 status queries passed.
- Pure host tests cover mouse report parsing, per-peer crypto/replay and input merging. The standalone app builds with the pinned NCS/SDK and required ESB patch.

These are functional checks, not input-latency or RF loss-rate measurements. There was no long soak or repeated full fresh-pair cycle. New ZMKS UI polling and cancellation have unit/build checks; native UI selection/scan/connect was not fully automated in this final run. Private raw UF2 backups, pairing records, serial identities and personal keymaps are intentionally not distributed.

## Update — 2026-11-15

- Hub 0.6.7-motion-breath built and was flashed to the same receiver. Application hash/identity were checked, pairing was retained, and retained fault count was zero. The two keyboards reported UNKNOWN at that check; physical LED appearance was not separately confirmed.
- Host tests cover link double flashes, per-peer READY/LOCKED/UNKNOWN parsing, 1250 ms freshness and clearing on disconnect, mixed/all lock aggregation and brightness calculations, in addition to the existing mouse, crypto/replay and input-merge tests.
- EVO preview.3/4 policy tests and user feedback covered the brief-flat-while-moving failure and improved blocking. Preview.5 retains those defaults and adds settings. Its application build, policy/bounds/parameter-effects tests, reset behavior and actual heartbeat sender tests passed. Settings persistence across a physical reboot and changed-parameter input behavior remain pending user tests; preview.5 has not been flashed by the agent.
- ZMKs PR #12 now contains receiver-first connection, lift-lock state and settings support. It passed 22 tests / 120 assertions, type checks, production build and browser interactions. Browser simulations do not replace physical native dual-serial testing.

### Source-package rebuild check

The final PR directory was checked from fresh pinned core/config/messages checkouts: Studio patches and the optional Motion Guard overlay applied successfully, with baseline motion RPC/protocol hashes verified before mutation. EVO preview.5 (`ph_lite` / `PH60-SC_V2_EVO`) and Hub 0.6.7 (`promicro_nrf52840/nrf52840/uf2`) both rebuilt successfully from these proposed sources. All policy/settings, real reset/heartbeat harnesses, link/motion LED, mouse parser and peer crypto/input-merge host tests passed. This check did not flash either device.
