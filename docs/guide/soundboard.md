# Soundboard

The soundboard plays short files from the Pi's persistent storage. The browser,
a MIDI controller, and the optional touchscreen all use the same sound library.
Playing a sound generates audio at the soundboard source; it still needs a
valid [route](./routing) to a connected output.

## Play mode and Edit mode

The **Soundboard** page opens in **Play** mode. Its cards expose playback
controls without the file-management controls getting in the way.

Choose **Edit** to show:

- Collapsible **Add Sound** and **Layout & MIDI setup** panels.
- Compact cards with playback previews and an expandable **Settings** section
  for grid position, **Play/Hold**, **Assign MIDI**, **Clear MIDI mapping**,
  **Replace**, and **Remove**. Open settings stay open when the grid refreshes.
- **Refresh**, which reloads the file list and related state.
- Reordering controls, including keyboard-accessible sound names.

Return to **Play** for performance. Switching modes does not remove sounds or
clear their mappings.

## Upload a sound

1. Choose **Edit**.
2. Expand **Add Sound** and pick an audio file.
3. Optionally enter a descriptive filename, such as `intro.wav`.
4. Choose **Upload** and wait for conversion and the cache reload to complete.
5. Test the sound at a low output level.

The uploader converts non-filename-safe characters to underscores and ensures a
`.wav` extension. Prefer simple, unique names using letters, numbers, hyphens,
and underscores. Avoid differing only in capitalization.

AudioX processes uploads with FFmpeg into **48 kHz, stereo, 16-bit PCM WAV**.
Supported input formats depend on the FFmpeg binary included in your image;
MP3 and Ogg are examples, not a guarantee that every codec/container will work.
Check **System > Device Info > FFmpeg** if conversion fails.

The current `soundboard_out` routing source is **mono**. For a stereo file,
playback reads its first (left) channel; conversion to stereo does not create a
stereo soundboard bus. Prepare clips with the intended content in that channel,
then route the mono source to both output channels if you want it on both sides.

::: warning Short clips, not a streaming player
The current cache supports **64 loaded clips** and caps each clip at
`SAMPLE_RATE * 8` frames. With the normal 48 kHz build and converted uploads,
only the first **eight seconds** of a longer file are loaded. The file may
remain longer on storage, but that does not make the entire file playable.
Prepare short clips before uploading.
:::

The HTTP file-upload limit is **150 MiB per file**. This is a transfer limit,
not a promise that a file that large can be cached or played in full. Storage,
RAM, conversion time, and clip-length limits still apply.

## Play and Hold

**Play** starts a one-shot voice. Let it finish, or choose **Stop All** to stop
soundboard playback. Repeated triggers can overlap; this is not a toggle-to-stop
button.

**Hold** starts on press and stops on release, or when the clip reaches its end.
It does not loop the clip indefinitely. In the browser, you can hold with a
pointer or with Space/Enter on the focused button. Pointer cancellation,
leaving the button, or losing focus releases the browser hold.

There is one shared held-voice slot: starting a different held sound replaces
the held voice. One-shot voices can still coexist with it. For MIDI hold
playback, any note-off releases the held sound, including note-on messages with
velocity zero. This deliberately handles controllers with inconsistent release
note numbers; it is not independent polyphonic key holding.

**Stop All** stops soundboard voices, not every live audio route, effect, or
Lua macro. To stop unrelated live audio, change its route or volume.

## Map a MIDI pad

1. Connect the controller to the Pi.
2. In **Edit**, expand the sound's **Settings** and choose **Assign MIDI**.
3. Press the desired pad **after** entering assignment mode.
4. Check the note shown on the card.

Choose the assignment control again to cancel waiting. Use the small remove
button beside an assigned MIDI note to clear the mapping.

Ordinary soundboard mappings use a **note number**, not a note-plus-channel.
Macros use explicit channel/note bindings instead. Reserve separate note
numbers for sounds, Stop All, effect bypass, and Sampler Toggle where possible;
overlapping mappings can have precedence effects. Read [MIDI](./midi) before
building a shared controller layout.

## Reorder your library

In Edit mode:

- With a mouse, drag a card without grabbing a playback or editing control.
- On a touchscreen browser, drag the sound's **name**; the rest of the card can
  still be used to scroll.
- With a keyboard, focus a sound name, press **Space**, move it using the arrow
  keys, then press **Enter** to save. **Escape** cancels.

The order saves on the device and is shared with the
[local display](./display). The display refreshes within roughly two seconds
and avoids moving pads during an active touch. New sounds without saved
positions append in filename order. With no saved order, filename order is used.

If another client changes the file list while you reorder it, AudioX rejects a
stale order rather than discarding that change. Refresh and retry.
Older firmware may allow playback but not shared ordering; the WebUI explains
when ordering is unavailable.

## Normal and grid layouts

In **Edit**, expand **Layout & MIDI setup** and choose **Normal list** or **MIDI grid**.
Normal list is the default and keeps the existing reorder workflow. Grid mode
supports up to 8 rows by 8 columns, including empty cells. Switching an existing
list to grid places its sounds in their current order; it keeps their MIDI
assignments. If the grid is smaller than the library, remaining sounds appear
under **Unassigned sounds and actions**. New uploads are unassigned unless you choose a cell.
In Edit mode, drag any sound to an empty cell to move it. Drop onto another sound
to swap their positions; an unassigned sound displaces the target into the
unassigned list. On touchscreens, drag the sound name. Each card's **Settings**
contains a cell selector that
provides the same placement and swap controls for keyboard users.

Drag a sound into **Unassigned sounds and actions**, or choose **Unassigned** in its selector,
to remove its cell and sound-trigger MIDI mappings without deleting the file.
Unassigned sounds remain playable in the browser but do not occupy touchscreen
pads. Empty cells and unassigned sounds persist across reloads and layout-mode
switches. Shrinking the grid unassigns sounds outside its new bounds and clears
their sound-trigger mappings; resizing also resets MIDI corner learning.
The WebUI and touchscreen display use the same saved positions.

To match a physical MIDI pad grid, learn the **top-left**, **top-right**,
**bottom-left**, and **bottom-right** pads in Edit mode. Each button waits for
the next MIDI note. AudioX interpolates the notes between those corners and
requires a unique, evenly numbered grid; arbitrary note layouts cannot be
inferred from four corners. Once learned, assigning a sound to a grid cell also
assigns that cell's MIDI note to the sound. Moving a sound can therefore replace
another sound's mapping for that pad. Swaps update both sounds' MIDI assignments
and controller LEDs. Before calibration, grid moves preserve existing MIDI
mappings. When all four corners are learned, already placed MIDI-mapped sounds
are moved to their corresponding cells where possible; deliberately unassigned
sounds stay unassigned.

**Stop All** and **Sampler Toggle** are also available in the unassigned area.
Drag them into any cell or use their cell selectors; swaps with sounds and each
other work the same way. On a calibrated grid, placement moves the action's MIDI
binding to that pad and updates its lighting. Unassigning an action clears its
MIDI bindings but leaves the browser control available. Action tiles are also
shown and usable on the touchscreen. Sampler Toggle changes the runtime sample
selection mode described in [MIDI](./midi#play-a-sound-at-different-pitches), not
the saved sampler-keyboard enable setting.

## Replace and remove

**Replace** opens a file picker and uploads new audio under the existing
filename. This is useful when improving a clip while retaining mappings that
refer to that filename. Wait for conversion and bank reload before testing it.

**Remove** deletes the file. AudioX also clears that sound's note assignments,
per-sound lighting overrides, playback-mode override, and saved ordering entry.
Startup and soundboard reload clean up references to missing sound files too.

Lua source code is not rewritten when a clip is removed or renamed. A macro
that calls `sound.play("old.wav")` must be edited separately.

## Soundboard on the device display

Choose **Show Soundboard on Display** to use touch pads on the attached display.
The same button becomes **Show Meters on Display** to switch back. This choice
is saved. See [Display and touch](./display) for page navigation and hold behavior.

## Common problems

| Symptom | Check |
| --- | --- |
| The card plays but there is no audible output | Route the soundboard source to an output; check node volume and the output device |
| Playback ends after about eight seconds | Current clip-cache limit; shorten the source file |
| Upload fails or conversion fails | FFmpeg availability, supported input format, storage space, and the upload limit |
| MIDI pad selects a sound instead of playing it | Sampler selection mode may be active |
| A held sound stops when another note is released | Shared held-voice behavior; use one-shot mode if that fits better |
| A Lua sound action fails | The named WAV must already be loaded; refresh/reload the soundboard and check its exact filename |

For pitched playback, continue to [MIDI sampler controls](./midi#play-a-sound-at-different-pitches).
