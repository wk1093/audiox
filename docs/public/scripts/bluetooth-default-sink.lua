ui.option("enabled", {
    label = "Enable automatic switching", type = "boolean", default = true
})
ui.option("bluetooth_device", {
    label = "Bluetooth device", type = "bt-device", default = ""
})
ui.option("audiox_sink", {
    label = "AudioX desktop playback device", type = "host-device", default = ""
})
ui.option("retry_seconds", {
    label = "Retry interval (seconds)", type = "int", default = 5, min = 2, max = 60
})
ui.button("check", "Check now")

local function clear_session()
    state.active = nil
    state.ending = nil
    state.session = nil
    state.device = nil
    state.target = nil
    state.previous = nil
    state.override = nil
    state.next_try = nil
    state.requested = nil
end

local function request(name)
    if millis() < (state.next_try or 0) then return end
    desktop.set_default_sink(name)
    state.requested = name
    state.next_try = millis() + script_config.get("retry_seconds") * 1000
end

local function check(event, verbose)
    local enabled = script_config.get("enabled")
    local selected = script_config.get("bluetooth_device")
    local target = script_config.get("audiox_sink")
    local current, desktop_error = desktop.default_sink_status()
    if state.active and current and current.session ~= state.session then
        log("Desktop changed; forgetting the previous desktop's restore device")
        clear_session()
    end
    if current and event and event.type == "desktop_default_change"
        and event.desktop_session ~= current.session then return end

    if state.active and event and event.type == "desktop_default_change"
        and event.available and event.desktop_session == state.session
        and (not event.initial or
             (current and event.device == current.name and event.device ~= state.previous)) then
        if event.device ~= state.target then
            state.previous = event.device
            state.override = true
            state.next_try = nil
        else
            state.override = false
        end
    end
    if not current then
        if verbose then log("Desktop default sink unavailable: " .. (desktop_error or "check the companion logs")) end
        return
    end

    local configured = enabled and selected ~= "" and target ~= ""
    if verbose and not configured then
        log("Enable switching and select a Bluetooth device and playback sink")
    end
    local connected = false
    if configured then
        local bluetooth_status = bluetooth.devices()
        if not bluetooth_status.ready then
            if verbose then log("Bluetooth is not ready") end
            return
        end
        for _, device in ipairs(bluetooth_status.devices) do
            if device.address == selected and device.connected then
                connected = true
                break
            end
        end
        if verbose and configured then
            log(connected and "Bluetooth device is connected" or "Waiting for Bluetooth device")
        end
    end

    if state.active and
        (not connected or selected ~= state.device or target ~= state.target) then
        if not state.ending then state.next_try = nil end
        state.ending = true
    end

    if state.active and state.ending then
        if current.pending then return end
        if state.previous and current.name ~= state.previous
            and (current.name == state.target or current.name == state.requested) then
            request(state.previous)
            return
        end
        log("Bluetooth routing session ended")
        clear_session()
    end

    if not state.active and connected then
        if current.pending then return end
        state.active = true
        state.session = current.session
        state.device = selected
        state.target = target
        if current.name ~= target then state.previous = current.name end
        log("Bluetooth connected; selecting AudioX playback")
    end

    if not state.active or current.pending then return end
    if state.override then
        if state.previous and current.name ~= state.previous
            and (current.name == state.target or current.name == state.requested) then
            request(state.previous)
        end
    elseif current.name ~= state.target then
        request(state.target)
    end
end

hook("bluetooth_connect", check)
hook("bluetooth_disconnect", check)
hook("desktop_default_change", check)
hook("config_change", check)
hook("timer", check)
hook("run", function(event)
    state.next_try = nil
    check(event, true)
end)
hook("ui", function(event)
    if event.key == "check" then
        state.next_try = nil
        check(event, true)
    end
end)
