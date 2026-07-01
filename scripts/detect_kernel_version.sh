#!/bin/bash
# Auto-detect the kernel version from the firmware repo's uname_string8 file.
# Falls back to a default if the file doesn't exist or can't be parsed.

FIRMWARE_DIR="${1:-.}"
DEFAULT_KV="${2:-6.18.37-v8+}"

UNAME_FILE="$FIRMWARE_DIR/firmware/extra/uname_string8"

if [ -f "$UNAME_FILE" ]; then
    # Extract kernel version from "Linux version X.X.X-vX+ ..."
    # Pattern: "Linux version" followed by version string (up to first space)
    KV=$(sed -n 's/.*Linux version \([^ ]*\).*/\1/p' "$UNAME_FILE")
    
    if [ -n "$KV" ]; then
        echo "$KV"
        exit 0
    fi
fi

# Fallback to default
echo "$DEFAULT_KV"
exit 0
