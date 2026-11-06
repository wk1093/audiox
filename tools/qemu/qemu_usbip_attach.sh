#!/usr/bin/env bash
# Attaches the AudioX USB gadget exported by the QEMU debug VM (make debug) to
# this host with USB/IP, so it shows up as a USB audio interface plus the
# USB network adapter that serves the web UI at http://<CONFIG_DEVICE_IP>/.
#
# Usage: qemu_usbip_attach.sh [host] [port] [busid] [timeout_s] [watch]
#   watch=1 keeps running and reattaches whenever the VM drops the link (the
#   gadget is rebound on runtime restarts and audio setting changes).
#
# Needs the usbip userspace tool and the vhci-hcd kernel module on the host
# (Arch: pacman -S usbip; Debian/Ubuntu: apt install usbip or linux-tools-generic).
set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-3240}"
BUSID="${3:-usbip-vudc.0}"
TIMEOUT_S="${4:-300}"
WATCH="${5:-0}"
DEVICE_IP="${AUDIOX_DEVICE_IP:-172.31.73.1}"
# Match idVendor/idProduct and the NCM host_addr in src/init/gadget.cpp.
GADGET_VID="6666"
GADGET_PID="0ad0"
GADGET_HOST_MAC="02:00:00:00:00:01"
NM_PROFILE="audiox-qemu-usbip"
# Set SUDO="sudo -n" when credentials were cached beforehand (no tty for a prompt).
if [[ "${EUID}" -eq 0 ]]; then
    SUDO=""
else
    SUDO="${SUDO-sudo}"
fi

log() { printf '\r[usbip-attach] %s\n' "$*" >&2; }

USBIP="$(command -v usbip || true)"
if [[ -z "${USBIP}" ]]; then
    for candidate in /usr/sbin/usbip /sbin/usbip; do
        [[ -x "${candidate}" ]] && USBIP="${candidate}" && break
    done
fi
if [[ -z "${USBIP}" ]]; then
    log "usbip tool not found; install it (Arch: usbip, Debian/Ubuntu: usbip) and run 'make debug-attach'."
    exit 1
fi

if [[ ! -d /sys/devices/platform/vhci_hcd.0 ]]; then
    if ! ${SUDO} modprobe vhci-hcd; then
        log "could not load vhci-hcd; run 'sudo modprobe vhci-hcd' then 'make debug-attach'."
        exit 1
    fi
fi

# `usbip port` can't show the remote of an attachment without root (its records
# are root-only), so look for the gadget on a vhci_hcd port in sysfs instead.
already_attached() {
    local dev
    for dev in /sys/devices/platform/vhci_hcd.*/usb*/*-*; do
        [[ "$(cat "${dev}/idVendor" 2>/dev/null)" == "${GADGET_VID}" ]] || continue
        [[ "$(cat "${dev}/idProduct" 2>/dev/null)" == "${GADGET_PID}" ]] && return 0
    done
    return 1
}

wait_for_export() {
    local deadline=$(( SECONDS + $1 ))
    until "${USBIP}" --tcp-port "${PORT}" list -r "${HOST}" 2>/dev/null | grep -q "${BUSID}"; do
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 2
    done
}

attach() {
    if ! ${SUDO} "${USBIP}" --tcp-port "${PORT}" attach -r "${HOST}" -b "${BUSID}"; then
        return 1
    fi
    log "AudioX attached over USB/IP (USB audio interface + USB network)."
}

usbip_netdev() {
    local dev
    for dev in /sys/class/net/*; do
        [[ "$(cat "${dev}/address" 2>/dev/null)" == "${GADGET_HOST_MAC}" ]] || continue
        [[ "$(readlink -f "${dev}/device" 2>/dev/null)" == */vhci_hcd.* ]] || continue
        printf '%s\n' "${dev##*/}"
        return 0
    done
    return 1
}

nm() {
    nmcli "$@" >/dev/null 2>&1 || { [[ -n "${SUDO}" ]] && ${SUDO} nmcli "$@" >/dev/null 2>&1; }
}

# The host names the USB/IP NCM interface after its vhci port (for example
# enu1i3), so NetworkManager profiles pinned to the real device's usb0 never
# match it. Bring it up with an in-memory DHCP profile instead.
configure_network() {
    local iface="" state="" deadline=$(( SECONDS + 20 ))
    until iface="$(usbip_netdev)"; do
        if (( SECONDS >= deadline )); then
            log "USB network interface did not appear; use the forwarded web UI instead."
            return 0
        fi
        sleep 1
    done
    if ! command -v nmcli >/dev/null 2>&1 || [[ "$(nmcli -t -f RUNNING general 2>/dev/null)" != "running" ]]; then
        log "USB network is ${iface}; run a DHCP client on it to reach http://${DEVICE_IP}/."
        return 0
    fi
    # Wait for NetworkManager to manage the new device (it starts unmanaged/unavailable).
    deadline=$(( SECONDS + 10 ))
    while state="$(nmcli -g GENERAL.STATE device show "${iface}" 2>/dev/null)"; do
        case "${state%% *}" in
            100) log "USB network ${iface} already configured; web UI at http://${DEVICE_IP}/"; return 0 ;;
            10|20) (( SECONDS < deadline )) || break; sleep 1 ;;
            *) break ;;
        esac
    done
    nm connection delete "${NM_PROFILE}" || true
    if nm connection add type ethernet ifname "${iface}" con-name "${NM_PROFILE}" save no \
            ipv4.method auto ipv4.never-default yes ipv6.method disabled &&
        nm --wait 30 connection up "${NM_PROFILE}"; then
        log "USB network ${iface} is up; web UI at http://${DEVICE_IP}/"
    else
        log "could not start DHCP on ${iface} with NetworkManager; configure it manually to reach http://${DEVICE_IP}/."
    fi
}

if [[ "${WATCH}" != "1" ]]; then
    if already_attached; then
        log "${BUSID} is already attached from ${HOST}:${PORT}."
    else
        if ! wait_for_export "${TIMEOUT_S}"; then
            log "timed out waiting for ${BUSID} on ${HOST}:${PORT}."
            exit 1
        fi
        if ! attach; then
            log "attach failed; retry with 'make debug-attach'."
            exit 1
        fi
    fi
    configure_network
    exit 0
fi

# Watch mode: runs in the background of make debug until the VM exits.
if ! wait_for_export "${TIMEOUT_S}"; then
    log "timed out waiting for ${BUSID} on ${HOST}:${PORT}."
    exit 1
fi
while true; do
    if ! already_attached; then
        if wait_for_export 5 && attach; then
            configure_network
        else
            sleep 3
            continue
        fi
    fi
    # Keep cached sudo credentials fresh so later reattaches don't need a prompt.
    if [[ -n "${SUDO}" ]]; then
        ${SUDO} -v 2>/dev/null || true
    fi
    sleep 2
done
