-- Toggle a group of effects together from one MIDI pad and light the pad to match.
-- Edit EFFECTS, CHANNEL, and NOTE for your setup.
-- The on/off state is saved, so it survives restarts.
local EFFECTS = {"fx_9", "fx_10", "fx_11", "fx_12"} -- effect IDs to switch together
local CHANNEL = 0 -- lighting channel
local NOTE = 71   -- the pad this script is bound to

ui.option("enabled", {label = "Effects enabled", type = "boolean", default = true})
ui.option("on_light", {label = "Light when on", type = "int", default = 1, min = 0, max = 127})
ui.option("off_light", {label = "Light when off", type = "int", default = 0, min = 0, max = 127})

local function apply(enabled)
    for _, id in ipairs(EFFECTS) do effects.enable(id, enabled) end
    midi.note_on(CHANNEL, NOTE, script_config.get(enabled and "on_light" or "off_light"))
    log(enabled and "Effects on" or "Effects off")
end

local function toggle()
    local enabled = not script_config.get("enabled")
    script_config.set("enabled", enabled)
    apply(enabled)
end

hook("note_on", function(event)
    if event.pressed then toggle() end
end)
hook("run", toggle)
-- Restore the pad light (and effect states) when the controller reconnects.
hook("device_connect", function()
    apply(script_config.get("enabled"))
end)
hook("config_change", function()
    apply(script_config.get("enabled"))
end)
