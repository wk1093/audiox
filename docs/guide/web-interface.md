# Finding your way around AudioX

The AudioX WebUI is the control panel running **on your device**. Open it using
the address in [Getting started](./getting-started). This documentation site is
separate: reading a guide or an example here does not change your Pi.

Feeling lost? Choose **Need help? Start here** under the device version in the
WebUI header. It opens this guide in a new tab without leaving your current
controls or discarding unsaved edits. The documentation is hosted online, so
your browser needs internet access to open it.

The interface is designed for both a desktop browser and a phone or tablet.
On a narrow screen, scroll the navigation tabs horizontally to reach every page.
Only one page is visible at a time; switching pages does not stop audio.

## Page map

| Page | Use it for | Guide |
| --- | --- | --- |
| Routing | Connect audio channels, insert processing, and inspect node/route details | [Routing](./routing) |
| Bluetooth | Discover, pair, connect, disconnect, or forget audio devices | [Bluetooth](./bluetooth) |
| Effects | Adjust effect parameters, MIDI controls, bypass mappings, and lights | [Effects](./effects) |
| Volume | Set output gain for individual audio things and assign MIDI knobs | [Volume](./volume) |
| Soundboard | Upload, play, replace, order, and map short audio clips | [Soundboard](./soundboard) |
| MIDI | Assign global actions, configure pitched sample playback, and set controller lights | [MIDI](./midi) |
| Macros | Create named keyboard shortcuts or Lua automations; run them from the web or MIDI | [Macros](./macros) |
| Config | Change USB gadget settings and back up or restore your setup | [Config and backups](./config) |
| System | Inspect device information and logs, restart, shut down, and install firmware | [System](./system) |

## Understand what Save means

AudioX does not have a single global Save button.

- Drawn connections need **Save Routing**. Moving nodes and changing zoom are
  not the same as changing the saved signal path.
- **Config > Save Config** stages USB settings. **Reload Config** applies them
  and reconnects USB functions.
- Soundboard assignments, playback-mode changes, ordering, and the individual
  MIDI/effect mapping controls save through their own actions.
- A macro's **Save Macro** stores its definition. **Run** queues the saved
  version; it does not run unsaved editor text.
- Lua effect/volume actions, **Dim lights**, and a macro's `state` are runtime
  state, not permanent settings.

See each page's guide before relying on a change to survive restart. Save all
pending editor changes before making a [configuration backup](./config#back-up-your-setup).

## Notifications and long-running actions

Popup notifications report successful operations, validation failures, and lost
connections. Read the notification instead of repeatedly clicking a disabled
control. Device polling warnings clear after the corresponding operation starts
succeeding; other errors may need dismissal or a corrected action.

Destructive actions such as deleting a sound, effect, macro, or MIDI mapping ask
for confirmation before they are applied. If you have unsaved routing, USB
configuration, macro-editor, MIDI settings, or a staged sound upload, the
browser also asks before you close or reload the page. That browser warning is
generic, and its wording is controlled by the browser. Save or clear the pending
editor before leaving if you want to keep those changes.

Restart, shutdown, firmware installation, and backup restore use confirmation
dialogs. Read the operation name carefully: restoring a backup replaces your
saved setup, while firmware installation changes the software.

For operations that reboot, the loading dialog waits for a new device boot,
not just for the network to disappear and return. **Stop waiting** closes the
browser's wait; it does **not** cancel an update, restore, or reboot already
requested on the Pi. Reopen the device address when it is ready.

## A practical first session

1. Check **System** for the expected board and firmware version.
2. Upload one short clip on **Soundboard**, in **Edit** mode.
3. Use **Routing** to connect the soundboard source to your intended output.
4. Start with a low **Volume**, then return to Soundboard and test playback.
5. Add one effect and confirm its input and output are both connected.
6. Add MIDI mappings or a named macro only after the basic audio path works.
7. Download a configuration backup once the setup is useful.

This order separates audio-path problems from controller and automation
problems. Use the [troubleshooting checklist](./troubleshooting) if any step fails.

## Browser and device requirements

The device serves its control interface over HTTP. Treat it as a trusted local
control connection, not a public website to expose to the internet. Several
interface assets are fetched from external CDNs, so some visual or routing
functionality may depend on the browser being able to fetch those assets.
See [networking](./networking) for connection details and offline limitations.

A touchscreen is optional. Its local UI is a smaller playback and monitoring
surface, not a replacement for every configuration page:
[Display and touch](./display).
