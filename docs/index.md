# AudioX documentation

AudioX turns a Raspberry Pi into a flexible audio workstation with audio routing,
effects, MIDI controls, and a browser-based soundboard.

## Write a named automation macro

Use the [getting started guide](/lua/getting-started) to create a named macro on
the **Macros** page, run it from the web, or optionally bind a MIDI pad. Continue with
the [Lua API reference](/lua/api), or browse practical examples in the
[cookbook](/lua/cookbook).

> **Device feature, not browser automation:** Lua scripts run on AudioX firmware
> as queued automation macros. Only keyboard action plans need USB Keyboard
> enabled and a connected USB host; light/MIDI plans need the selected controller
> connected. Sound, effects and volume plans need neither device.
> This website is documentation only; it does not run scripts.

**Lua guides**

- [Getting started](/lua/getting-started)
- [Events and persistent state](/lua/events-and-state)
- [API reference](/lua/api)
- [Keyboard and lighting](/lua/keyboard-and-lighting)
- [MIDI output and soundboard](/lua/midi-and-soundboard)
- [Cookbook](/lua/cookbook)
- [Limits and troubleshooting](/lua/limits-and-troubleshooting)

## AudioX OS guides

Guides for installing AudioX, configuring audio routes and effects, using the
browser interface, and maintaining a device are planned. This documentation
currently covers Lua automation macros; the OS guides are not published yet.

For now, see the [project README](https://github.com/wk1093/audiox#readme) for
installation and device setup.
