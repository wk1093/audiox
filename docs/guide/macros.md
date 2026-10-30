# Macros and automation

The **Macros** page holds up to **16 named automations**. A macro can be a
single keyboard shortcut or a bounded Lua script. It can run from a web button,
an optional MIDI note-on binding, or both.

This is separate from the MIDI page: MIDI configures the controller's global
actions and lighting, while Macros defines reusable named behavior.

## Create and run a macro

1. Open **Macros** and choose **New Macro** if an existing entry is being edited.
2. Enter a recognizable name, such as `Quiet monitor` or `Open recording app`.
   Names allow up to **63 UTF-8 bytes**, not necessarily 63 visible characters.
3. Leave **Bind to a MIDI pad** unchecked for a web-only automation.
4. Choose **Keyboard chord** or **Lua sequence** and enter its content.
5. Choose **Save Macro**.
6. Choose **Run** beside the saved entry.

Run queues the **saved version**, not the unsaved editor. It confirms queuing,
not completion. Use **Reload Macros** to see a reported execution error, and
check [System logs](./system) for context.

Choose **Edit** beside a saved entry to modify it. Changing its name or MIDI
binding keeps the same stable ID. **Remove** deletes that definition.

## Record a shortcut

For **Keyboard chord**:

1. Enable the USB keyboard function in [Config](./config), save, and reload.
2. Connect AudioX's USB gadget port to the target computer.
3. Choose **Record Chord**, press the intended combination in the browser,
   then release it.
4. Check the captured text and choose **Save Macro**.
5. Focus the intended application on the target computer and trigger the macro.

The browser captures physical key positions, modifiers, and up to six
non-modifier keys. Supported keys include letters, digits, F1-F24, Enter,
Escape, Tab, Space, Backspace, Delete, arrows, and navigation keys. Modifiers
include Ctrl, Shift, Alt, and Meta/Windows.

Some combinations are intercepted by the browser or operating system. Enter
them manually, for example `CTRL+SHIFT+F13`. Recording is canceled on blur or
leaving the page so an abandoned recorder does not keep capturing keys.

Keyboard output goes to the **USB host**, not necessarily the machine running
the browser. It affects whichever host application currently has focus.

## Add a MIDI trigger

Check **Bind to a MIDI pad**. Enter a channel (0-15) and note (0-127), or choose
**Learn MIDI Pad** and press it. Choose Save Macro after learning.

Two macros cannot share the same channel/note binding. Web-only entries do not
reserve a MIDI trigger. A macro binding does not remove a separate sound/action
mapping: one pad can still queue a macro and perform another local action.

Only note-on triggers a script. Note-off does not run another script or cancel
the queued sequence. If you need stateful toggling, implement it inside Lua.

## Know which devices an action needs

| Action | Requirements |
| --- | --- |
| `tap`, `press`, `release`, `type`, or a plain chord | USB Keyboard enabled and a connected USB host |
| `light` | Enabled MIDI binding and selected controller connected |
| `midi.*` | Selected controller connected; output goes to that controller |
| `sound.*` | Audio subsystem and the named clip already loaded |
| `effects.*` and `volume.*` | Existing effect/audio thing IDs |
| Calculations, timing helpers, `state`, and logging | No USB host or MIDI controller required |

USB Keyboard is not a global enable switch for automation. A web-only
volume/effect script works with it disabled.

## Lighting

Set **Idle LED Velocity / Color** and **Pressed LED Velocity / Color** for a
MIDI-bound macro. Zero means off; 1-127 is interpreted by the controller.
The global LED output channel is configured on **MIDI > Lighting**.

For web-only macros, leave both defaults at zero. There is no bound pad for
`light()` to control. A script that calls `light()` without a binding reports
an error even when started with Run.

Valid macro colors override other mapped colors on the same note.
**Dim lights** still wins. Temporary scripted lighting is restored to the
ordinary state when playback completes or is canceled.

## Write Lua automations

Lua can combine keyboard, sound, MIDI, effects, and volume actions with
functions, conditionals, loops, arithmetic, and bounded standard-library helpers.
Start with the [Lua getting-started guide](/lua/getting-started), then use:

- [Events and state](/lua/events-and-state) for counters, toggles, and elapsed time.
- [API reference](/lua/api) for exact arguments and limits.
- [Cookbook](/lua/cookbook) for working patterns.
- [Limits and troubleshooting](/lua/limits-and-troubleshooting) before long scripts.

Effect/volume actions are **runtime-only**. They do not write permanent settings
on each trigger. Volume uses 0-100 percent, where 100 is unity; adjustments clamp
to that range. Effect parameters use their actual units/ranges, not a generic
0-1 scale.

Lua can explicitly enable/bypass an effect, toggle it, and set its parameters.
See [Controlling effects from Lua](./effects#control-effects-from-lua) for
parameter keys, valid ranges, and examples you can run from a named macro.

Use real IDs from the effect/routing data, not a display name or an invented
`master` volume. Lua route-changing automation is not implemented.

## Execution and state

Scripts briefly evaluate into an action plan; `wait()` delays playback, not Lua
evaluation. Trigger fields and time helpers are snapshots from the original
Run or note-on. Plans play sequentially through a bounded queue.

Definitions survive reboot, but Lua `state` is held only in RAM. Edits,
cancellation/failure, controller disconnect, and restart can reset it.
USB keyboard session changes reset keyboard-using state; keyboard-free scripts
are not globally reset just because the USB host disappears.

If evaluation fails, its queued actions and state changes are not applied.
Playback can still fail after earlier physical actions were delivered; those
cannot be undone. Do not build a macro that depends on automatic rollback of
keyboard input, a sent MIDI message, or an already changed volume.
