# Networking

The stock AudioX control network is a point-to-point **USB NCM** connection between the Pi and its host computer. Audio and networking share the composite USB gadget. The runtime configures the Pi-side `usb0` interface; the host's interface name is OS-dependent.

This is not a router or an internet-sharing setup. The Pi does not need internet to serve its UI, receive a local update, or process audio.

## Current defaults

| Setting | Default |
| --- | --- |
| Pi interface | `usb0` |
| Pi IPv4 address | `172.31.73.1` |
| Netmask | `255.255.255.0` (`/24`) |
| Host DHCP pool | `172.31.73.10`–`172.31.73.250` |
| Lease time | 12 hours |
| DNS server advertised to host | `172.31.73.1` |
| DNS search domain | `lan` |
| Router/default gateway option | Not advertised |
| HTTP UI | Port 80, plain HTTP |
| mDNS name | `audiox.local`, answered on `usb0` |

Older instructions using `169.254.1.2/16` describe the previous link-local setup. They do not match the current DHCP network.

## Connect with DHCP

1. Use the Pi's USB-C device connection and a data-capable cable. Verify adequate power; see [Hardware](/guide/hardware).
2. Wait for the host to discover the USB NCM adapter. A USB audio device and a USB network adapter are separate interfaces: seeing one does not prove that the other's driver/profile is working.
3. Set that adapter's IPv4 method to **Automatic (DHCP)**. Do not configure internet sharing, a bridge, or a manual default gateway for this link.
4. Check that the host receives a `172.31.73.x/24` address in the advertised pool.
5. Open **`http://audiox.local/`**. If it does not resolve, open **`http://172.31.73.1/`** directly.

AudioX's dnsmasq serves only `usb0`. It advertises DNS and a search domain, but deliberately omits a router option, so the DHCP lease does not install an AudioX default route. Host DNS priority is still controlled by the host OS, VPN, and other network profiles.

### Linux with NetworkManager

In the network settings for the newly discovered adapter, select automatic IPv4/DHCP. Use `ip -br addr` or `nmcli device status` to identify the actual interface; it may be `enx...`, not `usb0`.

If you previously created the old link-local profile named `audiox-usb0`, remove it **only if it is that obsolete AudioX profile**:

```bash
sudo nmcli connection delete audiox-usb0
```

Then activate an automatic/DHCP connection for the current adapter. Do not remove unrelated profiles. A host profile pinned to a different interface name or MAC address may not apply after changing devices.

Linux name resolution for `.local` requires an enabled mDNS path, such as Avahi/nss-mdns or systemd-resolved with mDNS support. An IP connection can work even while this host service is missing.

### Windows and macOS

Enable DHCP/automatic IPv4 on the AudioX NCM network adapter and use the explicit HTTP URL. Do not assign the Pi's `.1` address to the computer.

The repository describes mDNS-capable host stacks, but Windows/macOS hardware validation is not established by its mDNS tests. If the adapter is absent, investigate NCM enumeration/driver support separately from DNS; this guide does not prescribe an unverified third-party driver or promise a particular Windows device name.

## Which hostname should I use?

### `http://audiox.local/`

This is the recommended name. AudioX runs its own mDNS responder on `usb0`, separate from ordinary DNS. It answers IPv4 A and reverse PTR queries, supports multicast and unicast-response queries, and announces on interface/address and host-link changes. It does not provide an IPv6 address.

The responder does not probe for hostname conflicts, relying on the direct one-host USB link. Bridging multiple devices together is not the intended topology.

### Other DNS names

The device's DNS server answers:

- `audiox`
- `audiox.lan`
- `audiox.local`
- `audiox.wklab.net`

All map to the device's default USB address. Requests for unrelated names are refused rather than forwarded upstream. Whether the host falls back to another DNS server is host-dependent; AudioX does not resolve the public internet for it.

Some hosts never query this DNS server because a VPN, another adapter, or a router using the same `lan` domain wins. That is why the independent mDNS responder exists. Include `http://`; a browser may interpret a bare `audiox` as a search.

### `http://audiox.wklab.net/`

This fallback name has a public DNS record pointing at `172.31.73.1`; the device's own DNS also answers it. It can help when a VPN intercepts `.local`, but public DNS lookup may need internet if the host does not ask AudioX.

- Browsers may show an HTTP/HTTPS-first warning for a public domain served over plain HTTP. Use the raw IP instead if you do not want that path.
- Some routers' DNS rebinding protection blocks public names pointing at private addresses.
- The name is not a cloud tunnel: HTTP traffic goes to the private USB address, not through an AudioX internet service.
- Without the USB route, the same private address could refer to another device on a conflicting network. Verify the System model/version before issuing maintenance actions.

Never bypass an unexpected certificate or unrelated-site warning on the assumption that it is AudioX.

## Manual static fallback

If there is no DHCP client on the host, assign an unused host address such as `172.31.73.2/24`. Keep the Pi at `.1`, and leave the host's gateway blank for this adapter.

On Linux, replace `HOST_IFACE` with the adapter name you identified:

```bash
sudo ip addr add 172.31.73.2/24 dev HOST_IFACE
sudo ip link set HOST_IFACE up
```

Then open `http://172.31.73.1/`. These `ip` commands are transient and may be overridden by NetworkManager or disappear on reconnect/reboot. Do not run them on the wrong adapter or assign an address already used by another machine.

Static access does not require device DNS. mDNS can work independently if supported by the host. A static fallback is useful for diagnosing dnsmasq, not a reason to run multiple competing network managers on the host.

## Troubleshoot in layers

1. **No USB adapter:** check power, device-port selection, data cable, and host NCM driver. DHCP settings cannot fix missing USB enumeration.
2. **Adapter but no IPv4 lease:** enable the host DHCP client, remove only obsolete link-local settings, and retry. A `169.254.x.x` self-assigned address is not a successful AudioX DHCP lease.
3. **IPv4 lease but no UI:** try the raw IP. Check for an overlapping `172.31.73.0/24` VPN/LAN route, a proxy, or host firewall policy. Do not disable all host protections as a first step.
4. **Raw IP works, name fails:** inspect mDNS and host DNS/VPN settings. Use the IP while troubleshooting rather than reflashing a working device.
5. **Connection drops after Config or restart:** USB function changes unbind/rebind the entire gadget, temporarily removing network and audio together. Wait for enumeration and lease recovery; reopen host audio/MIDI streams if required.
6. **Drops during an update:** preserve power. Let System finish its reboot wait and verify the installed version before retrying an upload.

For an API check from the connected host:

```bash
curl --fail --show-error http://172.31.73.1/api/system/info
```

If DHCP/DNS is unavailable, the watchdog may have continued after finding a missing dnsmasq binary/config, or dnsmasq may be restarting. Its messages go to the console, not necessarily the runtime log pane. See [System](/guide/system) and [Troubleshooting](/guide/troubleshooting).

## Wi-Fi, Ethernet, and remote access limits

The repository implements no stock Wi-Fi configuration page, SSID/password setup, Wi-Fi access-point mode, or Wi-Fi provisioning instructions. Its networking initialization sets up `usb0`; the shipped module seeds are not a complete wireless-client setup. Having wireless hardware on a Pi does not make Raspberry Pi OS Wi-Fi recipes valid for this initramfs image.

Do not assume an SSH server, interactive remote shell, `raspi-config`, `nmcli`, or `wpa_supplicant` is installed on AudioX. NetworkManager commands above run on the **Linux host computer**, not on the Pi. The supported control surface described here is the HTTP UI/API.

Likewise, there is no documented stock Ethernet DHCP-client setup. QEMU's virtio network is a development facility, not evidence that the physical Ethernet jack is configured.

The HTTP server binds IPv4 `INADDR_ANY` on port 80, not exclusively the USB address, and has **no authentication/login or TLS layer**. If you add another interface or a host bridge/forward, you may make firmware, file, and power controls reachable there too. Keep this link trusted; do not expose port 80 or bridge it into an untrusted network.

## Changing the subnet when building

This is a build-time setting, not a System-page field:

1. Change **Network > Device USB network address** in `make menuconfig` (`CONFIG_DEVICE_IP`).
2. Update `root/etc/dnsmasq.conf` to match: host record, DHCP pool/subnet, and advertised DNS address.
3. Rebuild and validate with `make test-dnsmasq-config`; the build checks consistency.
4. Update host static profiles and upload targets. `PI_HOST` defaults to the configured device address, but an existing device still uses its currently installed address.
5. Remember that the public `audiox.wklab.net` record still points at the default address; it will not follow your private build's new subnet.

See [Development](/guide/development) for build and upload safety, and [Config](/guide/config) for USB function settings.

## Implementation references

Defaults and behavior come from `Kconfig`, `include/defs.hpp`, `root/etc/dnsmasq.conf`, `src/http/network.cpp`, `src/http/mdns.cpp`, `src/http/server.cpp`, `src/init/watchdog.cpp`, and `tests/mdns_test.cpp`. QEMU-specific networking is documented separately in [Development](/guide/development#qemu-debugging).
