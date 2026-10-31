# Hardware and cabling

AudioX currently targets a Raspberry Pi running a custom software image with USB audio/network gadget support. The proposed reverse-HAT audio board, microphone preamps, instrument inputs, dedicated headphone/monitor outputs, patch bays, and phantom power are **future hardware plans**, not connectors or features supplied by today's software image.

## Board status

| Board | Repository status | Build defaults |
| --- | --- | --- |
| Raspberry Pi 4 Model B | Hardware-tested target; not a claim that every peripheral or host combination is tested | `make pi4_defconfig` (also `make defconfig`), `kernel8.img`, `fe980000.usb`, `vc4-fkms-v3d-pi4` |
| Raspberry Pi 5 | Hardware-tested target; framebuffer UI remains untested with a connected display | `make pi5_defconfig`, `kernel_2712.img`, `1000480000.usb`, `vc4-kms-v3d-pi5` |
| Other Raspberry Pi models/Compute Modules | No matching board preset or release path documented here | Do not assume the Pi 4 image is compatible |

The kernel, device trees, firmware, overlays, USB controller, and modules must match the board. Pi 5's preset is not simply a faster Pi 4 preset: it uses different boot/display/controller defaults. Do not rename a Pi 4 download to make it look like a Pi 5 build.

USB host compatibility also needs care. Linux wire-level mDNS behavior has been tested, but Windows/macOS hardware validation for mDNS remains unverified. Physical gadget enumeration, USB MIDI/keyboard behavior, simultaneous multichannel audio, and host volume behavior require hardware checks; passing host unit tests or booting QEMU is not equivalent.

## Power and port roles

### USB-C: the host-facing device link

The build configures `dwc2` in **peripheral** mode and binds a composite USB gadget. Use the Pi's USB-C connection for the intended host-facing data link, rather than trying to connect a computer to one of the Pi's USB-A host ports.

Over that one data link AudioX can expose:

- Bidirectional USB Audio Class 2 audio.
- USB NCM networking for the browser UI, DHCP/DNS, and mDNS.
- Optional USB MIDI passthrough.
- Optional HID keyboard output for keyboard macros.

MIDI and keyboard functions are disabled unless configured. See [Config](/guide/config), [MIDI](/guide/midi), and [Macros](/guide/macros).

The Pi 5 USB device path has been tested on hardware. Use the board-matched preset and a suitable power/data arrangement; this does not establish compatibility with every cable, hub, or power supply.

**Use a data-capable cable.** Charge-only cables can power a board while leaving both audio and the network absent. A cable/adapter must support the host/device connection, not just fit mechanically. Do not use USB-A-to-USB-A cables to join the two machines' host ports.

### Power is a separate engineering requirement

The USB-C connection is also a power connection. AudioX does not make an arbitrary laptop port an adequate Pi power source. Allow for the board, display, USB audio interface, MIDI controller, and other peripherals when choosing power.

- Use a power supply and power/data arrangement appropriate to the exact Pi model.
- Do not assume any USB hub injects power into the device-side Pi connection or safely combines two supplies.
- Do not improvise a Y cable or connect competing power sources without a documented, safe hardware arrangement.
- Power off before changing internal ribbon cables, GPIO wiring, or the SD card.
- Keep power and the USB data connection stable throughout an update. A browser reconnect message is not permission to unplug.

The repository does not specify a universally validated powered-hub, USB-C splitter, or GPIO power-injection design. Follow the board/peripheral manufacturers' electrical instructions rather than treating this guide as such a design.

### USB-A: attach peripherals to the Pi

The Pi's USB-A ports are host ports for devices such as a USB audio interface, MIDI controller, or USB touch input. These are distinct from the host-facing gadget connection.

The runtime includes ALSA USB audio/raw-MIDI support and rediscovers devices, but a module in the build is not a tested-device compatibility list. Check the [Routing](/guide/routing) graph after plugging in an interface; its channels, names, and supported formats may differ from another interface.

Use powered peripherals or an appropriate powered hub when required. Begin with a minimal setup, then add one device at a time so power, enumeration, and audio problems can be separated.

### Audio connectors

Microphones, instruments, speakers, and headphones require suitable audio hardware and levels. The software does not turn a GPIO pin or an unspecified board connector into a protected analog input, nor supply microphone phantom power.

Do not assume built-in analog or HDMI audio has a preconfigured AudioX route just because a connector exists on the board. Discover the endpoints the running build actually exposes and route them explicitly. Start with low monitoring levels, check [Volume](/guide/volume), and avoid feeding an output back into its own input.

### Ethernet and Wi-Fi

The normal control path implemented here is the **USB NCM gadget**, not the physical Ethernet jack. The runtime configures `usb0`; the stock configuration has no documented Ethernet DHCP-client or Wi-Fi provisioning workflow. The board's presence of Ethernet/Wi-Fi hardware is not a promise that those interfaces are usable in this image. See [Networking](/guide/networking).

## Display and touch

A display is optional: audio, MIDI, and the Web UI continue running headlessly.

- For HDMI, use the board's display connector and a suitable cable; the Pi 4/5 display connectors are micro-HDMI, not full-size HDMI.
- HDMI video alone does not provide touch input. A touch display may need a separate USB touch connection and its own power.
- For a DSI panel, use the cable/adapter appropriate to the board and panel. Pi 4 and Pi 5 camera/display ribbon connectors are not interchangeable without the correct cable.
- Connect ribbons only with power removed. Follow the panel's orientation, latch, and power instructions.

The build includes Raspberry Pi display/touchscreen and selected USB/I²C touch driver families. An optional `CONFIG_DSI_TOUCH_OVERLAY` can add a panel overlay; its default is empty. Inclusion of a driver or automatic detection does not certify every panel.

Pi 4 defaults to FKMS; Pi 5 defaults to full KMS. Do not transfer a Pi 4 display overlay blindly to Pi 5.

### Headless and reconnect behavior

The local UI checks availability at roughly 500 ms intervals. On detected display loss, it releases framebuffer resources, suspends touch scanning, cancels gestures, and releases a sound held by touch. It reopens a usable display when it returns, without restarting audio.

Cable-loss detection depends on the framebuffer's driver. DRM connector status is used when available; on non-DRM panels, a still-present framebuffer may not reveal that the physical cable is absent. Shared framebuffer devices remain active if any relevant connector reports connected. Physical HDMI/DSI reconnect behavior still needs target-side validation.

## A safe bring-up sequence

1. Flash the matching image as described in [Getting started](/guide/getting-started).
2. With power off, insert the card and connect any required display/touch wiring.
3. Establish suitable power and the USB-C data link to the host.
4. Check the NCM network and open the Web UI before adding audio/MIDI peripherals.
5. Add one peripheral, wait for discovery, then inspect Routing or MIDI.
6. Test a simple audio route quietly before adding effects or increasing gain.
7. Save settings, create a [configuration backup](/guide/config), and use [System shutdown](/guide/system#device-actions) before removing power.

If nothing enumerates, eliminate the data cable and power arrangement as causes before changing firmware. If a device disappears under load, collect System logs and note the complete power/peripheral arrangement in the report.

## Serial diagnostics

Advanced kernel-fault investigation can use a **3.3 V UART adapter**, never RS-232 or 5 V signaling. Connect ground and the selected Pi UART TX to the adapter RX; do not connect the adapter's power pin. Confirm the UART/pins for the board and Bluetooth overlay before wiring.

The normal command line uses framebuffer consoles. Serial logging requires an appropriate console option, normally `console=serial0,115200`, while preserving the single-line command line and `rdinit=/init`. On the Pi 4, Bluetooth keeps the PL011 UART, so `serial0` (GPIO 14/15) is the mini UART. The optional `CONFIG_BT_MINIUART` setting swaps them, but it drops incoming Bluetooth audio packets and is not recommended. Pi 5 has different UART defaults. This is diagnostic work, not a guaranteed login shell. See [Troubleshooting](/guide/troubleshooting) and [Development](/guide/development) before modifying boot files.

## Implementation references

Hardware scope and defaults are grounded in `Kconfig`, `Makefile`'s generated boot configuration, `depmod.txt`, `bootmod.txt`, `src/init/gadget.cpp`, `src/framebuffer/context.cpp`, and the hardware/testing limitations in `README.md` and `TODO.md`. No additional physical hardware validation is claimed by this guide.
