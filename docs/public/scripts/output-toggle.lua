-- Switch between two outputs, each muted by its own Cut effect.
-- Bind this script to a MIDI note or CC button; Run and the Toggle button also work.
ui.option("headphones_cut", {label = "Headphones Cut effect ID", type = "string", default = "fx_1"})
ui.option("speakers_cut", {label = "Speakers Cut effect ID", type = "string", default = "fx_2"})
ui.button("toggle", "Toggle output")

local function apply(speakers)
    -- A Cut that is enabled mutes its path.
    effects.enable(script_config.get("headphones_cut"), speakers)
    effects.enable(script_config.get("speakers_cut"), not speakers)
    state.speakers = speakers
    log(speakers and "Output: speakers" or "Output: headphones")
end

local function toggle()
    apply(not state.speakers)
end

hook("note_on", function(event)
    if event.pressed then toggle() end
end)
hook("cc", function(event)
    if event.value >= 64 then toggle() end
end)
hook("run", toggle)
hook("ui", function(event)
    if event.key == "toggle" then toggle() end
end)
-- state is RAM-only, so the outputs return to headphones after a restart.
hook("boot", function()
    apply(state.speakers == true)
end)
