#!/usr/bin/env bash
# Host test: the PID 1 watchdog restarts a crashing helper service and keeps a
# healthy one alive across runtime restarts (separate process group).
set -euo pipefail

WATCHDOG="${1:?usage: watchdog_service_test.sh <watchdog> <hung-child> <work-dir>}"
HUNG_CHILD="${2:?usage: watchdog_service_test.sh <watchdog> <hung-child> <work-dir>}"
WORK_DIR="${3:?usage: watchdog_service_test.sh <watchdog> <hung-child> <work-dir>}"

rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"
CONF="${WORK_DIR}/service.conf"
: > "${CONF}"

cat > "${WORK_DIR}/crashing-service" <<EOF
#!/bin/sh
echo "start \$*" >> "${WORK_DIR}/crashing.starts"
exit 1
EOF
cat > "${WORK_DIR}/healthy-service" <<EOF
#!/bin/sh
echo "start \$*" >> "${WORK_DIR}/healthy.starts"
echo "\$\$" > "${WORK_DIR}/healthy.pid"
exec sleep 60
EOF
chmod +x "${WORK_DIR}/crashing-service" "${WORK_DIR}/healthy-service"

fail() {
    echo "watchdog service test failed: $*" >&2
    exit 1
}

"${WATCHDOG}" "${HUNG_CHILD}" 3 "${WORK_DIR}/crashing-service" "${CONF}" > "${WORK_DIR}/crashing.log" 2>&1
crash_starts=$(wc -l < "${WORK_DIR}/crashing.starts")
[[ "${crash_starts}" -ge 3 ]] || fail "crashing service started ${crash_starts} time(s), expected >= 3"
grep -q -- "--keep-in-foreground --conf-file=${CONF}" "${WORK_DIR}/crashing.starts" || fail "service arguments missing"
grep -q "dnsmasq pid=[0-9]* exited with status 1" "${WORK_DIR}/crashing.log" || fail "service exit not logged"

"${WATCHDOG}" "${HUNG_CHILD}" 2 "${WORK_DIR}/healthy-service" "${CONF}" > "${WORK_DIR}/healthy.log" 2>&1
healthy_starts=$(wc -l < "${WORK_DIR}/healthy.starts")
[[ "${healthy_starts}" -eq 1 ]] || fail "healthy service started ${healthy_starts} time(s) across runtime restarts, expected 1"
[[ "$(grep -c "main-loop heartbeat timed out" "${WORK_DIR}/healthy.log")" -eq 2 ]] || fail "runtime was not restarted twice"
if grep -q "dnsmasq pid=[0-9]* \(exited\|terminated\)" "${WORK_DIR}/healthy.log"; then
    fail "runtime kill reached the service"
fi
if kill -0 "$(cat "${WORK_DIR}/healthy.pid")" 2>/dev/null; then
    fail "service left running after watchdog exit"
fi

"${WATCHDOG}" /bin/false 1 "${WORK_DIR}/missing-service" "${CONF}" > "${WORK_DIR}/missing.log" 2>&1
grep -q "dnsmasq unavailable" "${WORK_DIR}/missing.log" || fail "missing service not reported"

echo "watchdog service supervision tests passed"
