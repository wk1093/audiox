local script_path = "docs/public/scripts/bluetooth-default-sink.lua"
local settings, state, status, connected, ready, now, actions
local device = "AA:BB:CC:DD:EE:FF"

local function reset(name)
    settings = {enabled = true, bluetooth_device = device,
                audiox_sink = "sink.audiox", retry_seconds = 5}
    state = {}
    status = {name = name or "sink.original", session = 1, pending = false}
    connected, ready, now, actions = false, true, 1000, {}
end

local function dispatch(kind, extra)
    local handlers = {}
    local event = extra or {}
    event.type = kind
    local environment = setmetatable({
        state = state,
        ui = {
            option = function() end,
            button = function() end
        },
        hook = function(name, callback) handlers[name] = callback end,
        script_config = {get = function(key) return settings[key] end},
        bluetooth = {devices = function()
            return {ready = ready, devices = {{address = device, connected = connected}}}
        end},
        desktop = {
            default_sink_status = function() return status end,
            set_default_sink = function(name)
                assert(status and not status.pending)
                status.pending = true
                actions[#actions + 1] = name
            end
        },
        millis = function() return now end,
        log = function() end
    }, {__index = _G})
    assert(loadfile(script_path, "t", environment))()
    if handlers[kind] then handlers[kind](event) end
end

local function finish(success)
    assert(status.pending)
    status.pending = false
    if success then status.name = actions[#actions] end
end

local function change(name, session)
    status.name = name
    dispatch("desktop_default_change", {
        available = true, initial = false, device = name,
        desktop_session = session or status.session
    })
end

reset()
dispatch("config_change")
assert(#actions == 0)
connected = true
dispatch("bluetooth_connect")
assert(actions[1] == "sink.audiox" and state.previous == "sink.original")
connected = false
dispatch("bluetooth_disconnect")
assert(#actions == 1 and state.ending)
finish(true)
dispatch("timer")
assert(actions[2] == "sink.original")
finish(true)
dispatch("timer")
assert(not state.active and status.name == "sink.original")

reset()
connected = true
dispatch("bluetooth_connect")
finish(true)
change("sink.headphones")
assert(#actions == 1 and state.previous == "sink.headphones" and state.override)
connected = false
dispatch("bluetooth_disconnect")
assert(#actions == 1 and not state.active and status.name == "sink.headphones")

reset()
connected = true
dispatch("bluetooth_connect")
change("sink.headphones")
finish(true)
dispatch("timer")
assert(actions[2] == "sink.headphones")
finish(true)
connected = false
dispatch("bluetooth_disconnect")
assert(#actions == 2 and not state.active)

reset()
connected = true
dispatch("bluetooth_connect")
finish(false)
now = now + 4999
dispatch("timer")
assert(#actions == 1)
now = now + 1
dispatch("timer")
assert(actions[2] == "sink.audiox")
finish(true)
settings.enabled = false
dispatch("config_change")
assert(actions[3] == "sink.original")
finish(true)
dispatch("timer")
assert(not state.active)

reset()
connected = true
dispatch("bluetooth_connect")
finish(true)
connected = false
status = {name = "sink.other_pc", session = 2, pending = false}
dispatch("timer")
assert(#actions == 1 and not state.active and status.name == "sink.other_pc")

reset("sink.audiox")
connected = true
dispatch("bluetooth_connect")
connected = false
dispatch("bluetooth_disconnect")
assert(#actions == 0 and not state.active)

reset()
connected = true
ready = false
dispatch("bluetooth_connect")
assert(#actions == 0 and not state.active)
ready = true
dispatch("timer")
finish(true)
local saved = status
status = nil
connected = false
dispatch("bluetooth_disconnect")
assert(state.active)
status = saved
dispatch("timer")
assert(actions[2] == "sink.original")

reset()
connected = true
dispatch("bluetooth_connect")
finish(true)
settings.audiox_sink = "sink.other"
dispatch("config_change")
assert(actions[2] == "sink.original")
finish(true)
dispatch("timer")
assert(actions[3] == "sink.other" and state.previous == "sink.original")

reset()
connected = true
dispatch("bluetooth_connect")
finish(true)
ready = false
change("sink.headphones")
assert(state.previous == "sink.headphones" and state.override)
ready = true
dispatch("timer")
connected = false
dispatch("bluetooth_disconnect")
assert(#actions == 1 and status.name == "sink.headphones")

reset()
connected = true
dispatch("bluetooth_connect")
finish(true)
dispatch("desktop_default_change", {
    available = true, initial = false, device = "sink.old_desktop",
    desktop_session = 99
})
assert(state.previous == "sink.original" and not state.override)

reset()
connected = true
dispatch("bluetooth_connect")
finish(true)
connected = false
dispatch("bluetooth_disconnect")
assert(actions[2] == "sink.original")
change("sink.headphones")
finish(true)
dispatch("timer")
assert(actions[3] == "sink.headphones")
finish(true)
dispatch("timer")
assert(not state.active and status.name == "sink.headphones")
