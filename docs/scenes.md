# Setup scenes

Open **Scenes** below the Routing graph to capture up to 32 named scenes.
The menu is collapsed by default and hidden in graph fullscreen. A scene includes all saved
routes (including currently disconnected devices), the routing helper, live
node gains, the **complete effect inventory** with stable IDs and display names,
bypass states, and every effect parameter. Scenes can therefore have different
numbers or types of effects. Save any WebUI routing draft before capturing;
live MIDI parameter and volume changes are captured even if not yet persisted.

If MIDI recalls a scene while a browser has unsaved wire edits, those edits
are retained and automatic graph refresh pauses. Download the wire draft if
needed, then choose **Load Active Routing** to load the current device setup.
The UI blocks saving the stale draft until it has been refreshed.
Capture, update and WebUI recall are blocked while routing has unsaved edits.

Capture a new scene, update an existing capture, recall it, or delete it from
Setup scenes. Names must be unique and at most 63 UTF-8 bytes. Recall replaces
the current routing/effects/gains; it does not merge inventories. The active
marker denotes the last recalled scene, not a guarantee the setup is unchanged.
Deleting a scene does not change currently playing audio.

For MIDI recall, enter a channel (1–16) and note (0–127), then press **Bind MIDI**.
Blanking the note and binding again removes the assignment. A nonzero-velocity
Note On on that channel recalls the scene; Note Off and other channels do not.
Scene notes take priority over soundboard/effect-toggle notes, so reserve distinct
notes. A channel/note pair can only belong to one scene. Binding changes are
detected without restarting MIDI. MIDI passthrough behavior is unchanged.

Recall fades all playback sinks to zero over approximately 10 ms, swaps the
effect inventory and routing while silent, and fades back over approximately
10 ms after the audio thread observes the new graph. Disk writes or plugin
startup can extend the silent interval. This is a gain transition, not a
crossfade of effect tails; DSP history is not part of a scene. If the audio
thread cannot acknowledge silence within 250 ms, recall fails without changing
the setup. Disconnected device routes remain stored but cannot sound until the
device returns. Custom effect plugins must remain installed.

## Storage and compatibility

Scenes are independent versioned files in `/audiox/config/scenes/<id>.scene`, written
using a same-directory staging file, file fsync, atomic rename and directory
fsync. An unsupported scene version is rejected rather than overwritten.
Scenes are optional. Successful recall updates
`routing.txt`, `volumes.txt`, `midi_map.txt` and `fx_params.txt` under
`/audiox/config/`, preserving
unrelated MIDI and main configuration settings, so the recalled setup survives
restart. Failed writes attempt to restore the previous config; these four
configuration files are not one crash-atomic transaction. Keep a configuration backup
before important performances. Scene files are included in `.axb` exports and
validated during restore. Pending scene writes and unrelated files in the scenes
directory are excluded.

## HTTP API

All endpoints return JSON. Scene IDs are 1–48 ASCII letters, digits, `_` or `-`.
Mutation fields are URL query parameters (not JSON request bodies):

| Method | Endpoint | Query |
| --- | --- | --- |
| GET | `/api/scenes` | None; returns `scenes`, last `active` ID and recall `sequence` |
| POST | `/api/scenes/save` | `id`, `name_hex` (hex UTF-8), `channel`, `note`; captures live state |
| POST | `/api/scenes/bind` | `id`, `channel`, `note`; changes binding only |
| POST | `/api/scenes/recall` | `id` |
| DELETE or POST | `/api/scenes/delete` | `id` |

API MIDI channels are **zero-based** (0–15). Use `channel=-1&note=-1` for
unbound scenes. Saving an existing ID explicitly replaces that capture.

Focused checks: `make test-scenes test-audio-runtime` and
`node --test tests/web/web_scenes_test.js`.
