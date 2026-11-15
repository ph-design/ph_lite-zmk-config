#!/usr/bin/env sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hub-tests.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cd "$root"

run() {
    name=$1
    shift
    cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -fno-sanitize-recover=all -Isrc -Itinycrypt/include "$@" -o "$tmp/$name"
    "$tmp/$name"
}

run names tests/test-usb-port-names.c
run aes tests/test-aes.c tinycrypt/source/aes_encrypt.c tinycrypt/source/utils.c
run mouse src/mouse-map.c tests/test-mouse-map.c
run peers src/peer-crypto.c src/input-merge.c tests/test-two-peers.c \
    tinycrypt/source/aes_encrypt.c tinycrypt/source/ccm_mode.c tinycrypt/source/utils.c
run link tests/test-led-link.c
run motion tests/test-led-motion.c
python3 tests/test-axis.py
