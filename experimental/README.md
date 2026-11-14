# Wireless Hub prototype

`wireless-hub/` is a standalone NCS application for a Pro Micro / SuperMini nRF52840 receiver. It combines two EVO keyboards over encrypted ESB with one BLE HID mouse and exposes USB input plus wireless Studio management. It is built separately from the repository's normal ZMK configuration.

`studio-2g4/` contains the pinned EVO-side ZMK/config patches, protocol extension and source snapshots. These patches target their documented baseline, not an arbitrary current ZMK checkout.

This package does not change the default keyboard builds. Hardware validation and remaining limitations are documented in `wireless-hub/VALIDATION.md`.
