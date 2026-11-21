local linking = require ("linking-utils")
local function defaultMetadata (source)
  local manager = source:call ("get-object-manager", "metadata")
  return manager and manager:lookup {
    Constraint { "metadata.name", "=", "default" },
  }
end

SimpleEventHook {
  name = "audiox/announce-policy",
  interests = {
    EventInterest { Constraint { "event.type", "=", "node-added" } },
    EventInterest {
      Constraint { "event.type", "=", "metadata-added" },
      Constraint { "metadata.name", "=", "default" },
    },
  },
  execute = function (event)
    local metadata = defaultMetadata (event:get_source ())
    if metadata and not metadata:find (0, "audiox.policy") then
      metadata:set (0, "audiox.policy", "Spa:String", "1")
    end
  end,
}:register ()

SimpleEventHook {
  name = "audiox/protect-remap-target",
  before = "linking/find-defined-target",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-target" },
    },
  },
  execute = function (event)
    local source, om, item, props, flags, target = linking:unwrap_select_target_event (event)
    if target then return end
    local name = props ["node.name"] or ""
    if not name:match ("^output%.audiox_auto_") and
       not name:match ("^input%.audiox_auto_") then
      local metadata = defaultMetadata (source)
      local rules = metadata and metadata:find (0, "audiox.routing")
      if not rules then return end
      local node = item:get_associated_proxy ("node")
      if not node then return end
      local updated = JsonUtils.match_rules_update_properties (Json.Raw (rules), {
        ["application.name"] = node.properties ["application.name"],
        ["node.name"] = node.properties ["node.name"],
        ["media.class"] = node.properties ["media.class"],
      })
      local destination = updated ["audiox.target"]
      if not destination then return end
      target = om:lookup {
        type = "SiLinkable",
        Constraint { "node.name", "=", destination },
      }
      if target and linking.canLink (props, target) then
        flags.has_defined_target = true
        event:set_data ("target", target)
      end
      return
    end
    local requested = props ["target.object"]
    if not requested then return end
    target = om:lookup {
      type = "SiLinkable",
      Constraint { tonumber (requested) and "object.serial" or "node.name", "=", requested },
    }
    if target and linking.canLink (props, target) then
      flags.has_defined_target = true
      flags.has_node_defined_target = true
      event:set_data ("target", target)
    else
      event:stop_processing ()
    end
  end,
}:register ()
