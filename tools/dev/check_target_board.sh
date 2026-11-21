#!/usr/bin/env bash
set -euo pipefail

board="${1:?expected board required}"
host="${2:?Pi host required}"
port="${3:?Pi port required}"
pi4="${4:-}"
pi5="${5:-}"

case "$board" in
    pi4|pi5) ;;
    *) echo "Error: make dev requires a Pi 4 or Pi 5 build target; got '$board'." >&2; exit 1 ;;
esac
if { [ "$board" = pi4 ] && { [ "$pi4" != y ] || [ "$pi5" = y ]; }; } ||
   { [ "$board" = pi5 ] && { [ "$pi5" != y ] || [ "$pi4" = y ]; }; }; then
    echo "Error: build target '$board' does not match the selected board configuration." >&2
    exit 1
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "Error: jq is required to verify the target Pi before deployment." >&2
    exit 1
fi

url="http://$host:$port/api/system/info"
if ! info="$(curl --fail --silent --show-error --max-time 5 --max-filesize 65536 "$url")"; then
    echo "Error: cannot verify the target Pi at $url; no firmware will be uploaded." >&2
    exit 1
fi
if ! detected="$(printf '%s' "$info" | jq -ser '
    if length != 1 or (.[0] | type) != "object" then error("invalid system information") else .[0] end
    | . as $info
    | if (.model | type) != "string" then error("missing hardware model")
      elif (.model | test("^Raspberry Pi 4 Model B( |$)")) then "pi4"
      elif (.model | test("^Raspberry Pi 5 Model B( |$)")) then "pi5"
      else error("unrecognized hardware model") end
    | . as $hardware
    | if ($info | has("board")) and $info.board != $hardware then error("board and hardware model disagree")
      else $hardware end
')"; then
    echo "Error: cannot establish the target hardware from $url; no firmware will be uploaded." >&2
    exit 1
fi
if [ "$detected" != "$board" ]; then
    echo "Error: build target is $board but $url reports $detected; no firmware will be uploaded." >&2
    echo "Select the correct board with make ${detected}_defconfig and review your configuration." >&2
    exit 1
fi
echo "Verified target hardware: $detected at $host:$port."
