#!/bin/sh
# Build the locked Studio image for one Evo product configuration.
set -eu

case "${1:-}" in
    default) profile=config ;;
    radio) profile=variants/evo-2g4 ;;
    *) echo "Usage: $0 {default|radio} [build-directory]" >&2; exit 2 ;;
esac
product=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace=$(cd "$product" && west topdir)
cd "$workspace"
core=$(west list zmk -f '{abspath}')
zephyr=$(west list zephyr -f '{abspath}')
build=${2:-"$workspace/build/evo-4.1-$1"}

exec west build -b ph_lite_evo -s "$core/app" -d "$build" \
    -S studio-rpc-usb-uart -- \
    "-DZephyr_DIR=$zephyr/share/zephyr-package/cmake" \
    "-DSHIELD=PH60-SC_V2_EVO" "-DZMK_CONFIG=$product/$profile" \
    "-DZMK_EXTRA_MODULES=$product" \
    -DCONFIG_ZMK_STUDIO=y -DCONFIG_ZMK_STUDIO_LOCKING=y
