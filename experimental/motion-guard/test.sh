#!/usr/bin/env sh
set -eu
assets=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
testbin=$(mktemp "${TMPDIR:-/tmp}/motion-guard-test.XXXXXX")
trap 'rm -f "$testbin"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "$assets/test_guard.c" -o "$testbin"
"$testbin"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g "$assets/test_power.c" -o "$testbin"
"$testbin"
python3 "$assets/test_tap_runtime.py"
python3 "$assets/test_power_runtime.py"
if [ "$#" -gt 0 ]; then
    python3 "$assets/test_driver.py" "$1"
    python3 "$assets/test_reset.py" "$1"
    python3 "$assets/test_telemetry_sender.py" "$1"
fi
