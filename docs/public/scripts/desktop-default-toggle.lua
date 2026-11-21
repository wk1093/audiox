ui.option("target", {
    label = "Desktop playback device", type = "host-device", default = ""
})
ui.button("toggle", "Toggle desktop default")

hook("ui", function(event)
    if event.key ~= "toggle" then return end

    local current, err = desktop.default_sink_status()
    if not current then
        log("Desktop default sink unavailable: " .. (err or "check the companion logs"))
        return
    end
    if current.pending then
        log("A desktop default change is still pending; try again shortly")
        return
    end
    if state.session and state.session ~= current.session then
        state.previous = nil
        state.target = nil
        state.session = nil
    end

    if state.previous and current.name == state.target then
        desktop.set_default_sink(state.previous)
        log("Requested previous desktop default: " .. state.previous)
        return
    end

    local target = script_config.get("target")
    if not target or target == "" then
        log("Select a desktop playback device first")
        return
    end
    if current.name == target then
        log("Selected device is already the default; no previous default was saved")
        return
    end

    desktop.set_default_sink(target)
    state.previous = current.name
    state.target = target
    state.session = current.session
    log("Requested desktop default: " .. target)
end)
