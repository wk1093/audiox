local selected
local linkable = {}
package.preload["linking-utils"] = function ()
  return {
    unwrap_select_target_event = function (_, event)
      return { call = function () return nil end }, event.manager, nil, event.props, event.flags, event.target
    end,
    canLink = function (_, target) return target == linkable end,
  }
end
Constraint = function (constraint) return constraint end
EventInterest = function (interest) return interest end
SimpleEventHook = function (hook)
  selected = hook
  hook.register = function () end
  return hook
end
dofile("desktop/wireplumber/audiox-routing.lua")

local function event(name, requested, key, found)
  local result = {
    props = { ["node.name"] = name, ["target.object"] = requested },
    flags = {},
    manager = { lookup = function (_, interest)
      assert(interest[1][1] == key)
      assert(interest[1][3] == requested)
      return found and linkable or nil
    end },
    set_data = function (self, _, target) self.target = target end,
    stop_processing = function (self) self.stopped = true end,
  }
  selected.execute(result)
  return result
end

local playback = event("output.audiox_auto_1", "123", "object.serial", true)
assert(playback.target == linkable and playback.flags.has_node_defined_target)
local capture = event("input.audiox_auto_1", "my-source", "node.name", true)
assert(capture.target == linkable)
assert(event("output.audiox_auto_1", "123", "object.serial", false).stopped)
assert(not event("spotify", "123", "object.serial", true).target)
