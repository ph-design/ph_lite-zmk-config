# Experimental keyboard and Hub firmware

Current maintained combination: **keyboard preview.12 + Hub 0.6.9**. These optional
kits do not replace the repository's default ZMK build. Source submission and UF2
delivery are separate; the PR contains source, while test images are delivered in
the companion local test package.

| Directory | Responsibility | Build / tests |
| --- | --- | --- |
| [studio-2g4](studio-2g4/README.md) | Pinned keyboard radio/RPC bridge, pairing extension, bounded transport and USB FIFO handling | Apply first to the exact documented ZMK/config baseline |
| [motion-guard](motion-guard/README.md) | EVO guard/tap/tilt/axis; independent power; sensorless SC V2 / Multi V2; USB BL and RGB fades | Apply second with apply.py; test.sh against the applied tree |
| [wireless-hub](wireless-hub/README.md) | Dual keyboard ESB, BLE mouse, LEDs, joystick reports and distinct CDC names | Separate pinned NCS application; tests/run.sh |

Apply both keyboard overlay layers together. Do not copy historical standalone
source snapshots over the assembled tree. Keyboard and Hub SDKs are different;
keep separate build environments. Existing protocol fields and stored settings
remain versioned; experimental game mode is never persisted.

Current validation: the user confirmed preview.12 USB Studio connection works.
Hub 0.6.9 was flashed and Windows returned distinct interface names; peer settings
were retained and fault count was zero. These observations do not substitute for
physical input, game, battery, latency or long RF soak measurements. Detailed
limits are recorded in each kit and [Hub validation](wireless-hub/VALIDATION.md).
