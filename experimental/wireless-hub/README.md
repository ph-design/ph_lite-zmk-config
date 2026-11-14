# Dual-keyboard ESB / BLE HID Hub 0.6.7

Standalone NCS application for Pro Micro / SuperMini nRF52840 with a compatible nice!nano UF2 bootloader. Two independently paired EVO keyboards share one RF channel; a BLE HID mouse connects through MPSL radio timeslots. This is a prototype, not a Bluetooth HCI adapter.

USB exposes AT management CDC, wireless Studio CDC and keyboard / consumer / mouse HID. Windows assigns port numbers; identify interfaces rather than assuming a particular COM number.

## Build

Pinned dependencies:

| Project | Revision |
| --- | --- |
| sdk-nrf v3.4.0 | 99553055607b2e9885fbc80ccd11fa9da81c2df0 |
| sdk-zephyr | bf801e4e3d19e1ffa76164346480cb7734dd2800 |
| sdk-nrfxlib | d4ce5fe1a7d8af29bc01a4e1ddf5540ef65b6a3b |
| Zephyr SDK GNU ARM | 1.0.1 |

Initialize an NCS workspace from the pinned sdk-nrf manifest and activate its Python environment. Set `ZEPHYR_SDK_INSTALL_DIR` to SDK 1.0.1. Apply `patches/esb-rx-extension-limit.patch` from the sdk-nrf repository root before building:

```sh
git -C /path/to/ncs/nrf apply --check /path/to/experimental/wireless-hub/patches/esb-rx-extension-limit.patch
git -C /path/to/ncs/nrf apply /path/to/experimental/wireless-hub/patches/esb-rx-extension-limit.patch
cd /path/to/ncs
west build --no-sysbuild -b promicro_nrf52840/nrf52840/uf2 \
  -d build-eio-hub /path/to/experimental/wireless-hub
```

The output is `build-eio-hub/zephyr/zephyr.uf2`. The board must have a compatible memory layout: application starts at 0x26000, NVS is 0xec000..0xf4000, bootloader is 0xf4000..0x100000. Check the actual board and bootloader before flashing.

The ESB patch reduces continuous PRX timeslot extensions from 2000 to 32. The unmodified pinned driver reproducibly asserted in `ts_idle_action()` after BLE scanning stopped and long uninterrupted RX resumed. The patch bounds accumulated drift while retaining normal END/re-request handling and all state assertions. This is a workaround validated on the documented nRF52840 board; it is not an upstream fix for every SoC.

## Host tests

From this directory:

```sh
cc -std=c11 -Wall -Wextra -Werror -Isrc src/mouse-map.c tests/test-mouse-map.c -o /tmp/test-mouse-map
/tmp/test-mouse-map
cc -std=c11 -Wall -Wextra -Werror -Isrc -Itinycrypt/include \
  src/peer-crypto.c src/input-merge.c tests/test-two-peers.c \
  tinycrypt/source/aes_encrypt.c tinycrypt/source/ccm_mode.c tinycrypt/source/utils.c -o /tmp/test-two-peers
/tmp/test-two-peers
```

## Pairing and management

ZMKS provides receiver LED controls, BLE mouse scan/connect/forget and two keyboard slots. Pair each keyboard over USB and physically unlock Studio. The second keyboard needs the channel-setting RPC extension in `../studio-2g4/` so both peers can use the same RF channel with distinct keys and addresses.

To manage over radio, connect the receiver management interface in ZMKs first, then select an online keyboard to open its Studio interface. The keyboard stays on battery. Only one keyboard can be managed at a time; both can send input concurrently. Fn+backslash physically unlocks the selected EVO.

Studio CDC baud 115200 selects slot 0, 230400 selects slot 1. Slot 1 emits `HUB_TARGET=1\r\n` before binary RPC; clients must confirm this before sending requests. Slot 0 keeps the legacy binary-only stream. Slot mutation is refused while a Studio session is open.

AT commands use CRLF and return OK or ERROR:

- `AT+INFO`, `AT+GMR`, `AT+PEERS?`
- `AT+PEER=<0|1>`, `AT+PEERPAIR=<slot>,<32HEX key>,<10HEX addr>,<2HEX channel>`, `AT+PEERCLEAR=<0|1>`
- `AT+BLEPAIR` scans for 30 seconds; `AT+BLECONNECT=<0..7>` selects a fresh candidate; `AT+BLECLEAR` forgets the mouse; `AT+BLERESUME` sends HID Exit Suspend
- `AT+LED?`, `AT+LED=<AUTO|ON|OFF>,<0-100>,<5-50>,<500-5000>`, `AT+LEDSAVE`, `AT+LEDIDENTIFY`
- `AT+RST`, `AT+BL`

The factory compatibility key comes from the existing public firmware configuration. Generate independent per-device pairing keys for use; no private device pairing records are included here. AT lines containing keys must not be logged by clients.

## Input and recovery

Each peer has independent AES-CCM counters and replay protection. NKRO states are merged; consumer usages are unioned up to 12 entries. Disconnect/timeout releases only that peer. Mouse button state is tracked separately for both keyboards and BLE.

BLE HID subscribes only to mouse input reports and handles report IDs, non-byte-aligned fields, 12/16-bit movement, wheel and AC Pan. Report mode is explicitly written and Exit Suspend is sent during setup. Subscription removal withdraws READY. The red P0.15 user LED is PWM controlled; the hardware charging LED is separate.

BLE connection setup temporarily releases ESB timeslots, so keyboard input can pause briefly. Deep-sleeping keyboards must be physically woken before management. CDC uses a separate workqueue, and AT replies yield while sending long status output.

Fault diagnostics are RAM only. AT+INFO reports HUB_FAULT, HUB_ASSERT_FILE, HUB_ASSERT_LINE and HUB_SAFE_MODE. After a fault reset, safe mode leaves USB diagnostics available and skips ESB startup to avoid a reset loop repeatedly writing crypto epochs. Read the fault first, then use AT+RST to retry. Power loss may erase diagnostic records.

## Validation and limits

See `VALIDATION.md`. Long-duration stability, crowded RF latency, USB sleep/resume, other mouse models and other EIO/PHD keyboards remain unverified. This application does not forward PC keyboard lock LEDs and does not implement USB boot keyboard or remote wakeup.

TinyCrypt files retain Intel's BSD license notices. The ESB SDK patch is against Nordic's licensed SDK source; use the pinned SDK's license terms.

## Link and lift-lock indication (0.6.6 / 0.6.7)

AUTO mode adds two short 100 ms flashes on connect and two longer 300 ms flashes on disconnect. Per-peer encrypted motion status is reported as `HUB_MOTION=<0|1>,READY|LOCKED|UNKNOWN`; legacy packets, disconnect, startup and status older than 1250 ms resolve to UNKNOWN. Use EVO Motion Guard preview.4 or later for telemetry; preview.5 adds adjustable settings and remains wire compatible.

Only connected keyboards contribute to the persistent light pattern: none connected keeps waiting blink; no known locked peer keeps normal status; some locked gives shallow 65–100% breathing; all connected peers known locked gives full 5–100% breathing. Unknown is never counted as locked. Breathing has a fixed 2.4 s cycle, scaled by brightness. Identification, link-event flashes and low-battery alerts take priority. Mouse state does not enter the lock mask. Firmware keeps existing low-battery hysteresis.

Additional host checks:

```sh
cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test-led-link.c -o /tmp/test-led-link
/tmp/test-led-link
cc -std=c11 -Wall -Wextra -Werror -Isrc tests/test-led-motion.c -o /tmp/test-led-motion
/tmp/test-led-motion
```

`AT+BL` makes the USB-attached receiver enumerate its bootloader disk. It does not expose a battery-powered keyboard as a USB drive across the radio connection.
