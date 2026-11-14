#!/usr/bin/env sh
set -eu
assets=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
testbin=$(mktemp "${TMPDIR:-/tmp}/motion-guard-test.XXXXXX")
trap 'rm -f "$testbin"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "$assets/test_guard.c" -o "$testbin"
"$testbin"
if [ "$#" -gt 0 ]; then
    python3 "$assets/test_reset.py" "$1"
    python3 "$assets/test_telemetry_sender.py" "$1"
fi
