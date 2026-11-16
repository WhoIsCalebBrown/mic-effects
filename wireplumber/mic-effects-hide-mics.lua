-- Microphone Effects — hide the real microphones from apps.
--
-- Installed and enabled by the panel's "Hide raw microphone(s) from apps"
-- switch (see scripts/mic-effects-hide), never by default. It takes the
-- same permission route WirePlumber's own software-dsp script uses to hide the
-- hardware node behind a filter: every client except WirePlumber itself and
-- Microphone Effects loses the right to see the listed Audio/Source nodes, so the
-- only microphone left in their lists is "Microphone Effects" — while the
-- daemon can still read the real one.
--
-- Which nodes are hidden comes from the conf.d fragment that loads this
-- script:
--   mic-effects.hide-mics = { all = true }               all microphones
--   mic-effects.hide-mics = { nodes = [ "node.name" ] }  these ones
--
-- This is a courtesy boundary for well-behaved apps, exactly like the camera
-- side: anything that talks to PipeWire as its own client with its own name
-- can still ask for the raw device.

local log = Log.open_topic ("s-mic-effects")

-- This is loaded as a *required* component (the only flag WirePlumber honours
-- for a custom one — see mic-effects-hide), which means an error raised
-- at load time would stop WirePlumber from starting and take every audio
-- device with it. So nothing here may raise: the configuration is read inside
-- a pcall and anything unexpected simply means "hide nothing".
--
-- parse(2): the node list is one level below the object, and a shallower
-- parse would hand back the array as a Json object instead of a table.
local hide_all = false
local hide_names = {}

local ok, err = pcall (function ()
  local cfg = Conf.get_section_as_json ("mic-effects.hide-mics", Json.Object {}):parse (2)
  if type (cfg) ~= "table" then return end
  hide_all = cfg ["all"] == true
  local names = cfg ["nodes"]
  if names ~= nil and type (names) ~= "table" then names = names:parse () end
  if type (names) == "table" then
    for _, n in ipairs (names) do hide_names [tostring (n)] = true end
  end
end)
if not ok then
  log:info ("hide-mics: unreadable configuration, hiding nothing (" .. tostring (err) .. ")")
  hide_all = false
  hide_names = {}
end
log:info ("hide-mics: all=" .. tostring (hide_all))

local clients_om = ObjectManager { Interest { type = "client" } }
local nodes_om = ObjectManager { Interest { type = "node" } }
local hidden = {} -- bound-id -> object.serial for the microphones hidden last time

local function should_hide (node)
  local props = node.properties
  local class = props ["media.class"] or ""
  if class ~= "Audio/Source" and class ~= "Audio/Source/Virtual" then return false end
  local name = props ["node.name"] or ""
  if name:sub (1, 11) == "mic-effects" then return false end   -- our own virtual mic
  if hide_all then return true end
  return hide_names [name] == true
end

-- WirePlumber must keep full access (it does the linking), and so must the
-- Microphone Effects daemon (it reads the microphone it hides).
local function allowed (client)
  local props = client.properties
  if props ["wireplumber.daemon"] then return true end
  if props ["mic-effects.client"] == "true" then return true end
  return false
end

-- PipeWire clears a client's per-global permissions when that global is
-- destroyed. A later global may reuse the id, even with the same node.name.
-- Only restore a microphone if the very same global is still present.
local function reconcile (new_client)
  local ours = nodes_om:lookup {
    Constraint { "node.name", "=", "mic-effects", type = "pw-global" },
  } ~= nil
  local next_hidden, changes, all_denies = {}, {}, {}
  for node in nodes_om:iterate { type = "node" } do
    if should_hide (node) then
      local id = node ["bound-id"]
      -- An unbound proxy's id is PW_ID_ANY, the wildcard in permissions.
      if id ~= nil and id >= 0 and id < 0xffffffff then
        local serial = node.properties ["object.serial"] or node
        if ours then
          next_hidden [id] = serial
          all_denies [id] = "-"
          if hidden [id] ~= serial then changes [id] = "-" end
        elseif hidden [id] == serial then
          changes [id] = "rwxm"
        end
      end
    end
  end
  hidden = next_hidden
  if next (changes) then
    for client in clients_om:iterate { type = "client" } do
      if client ~= new_client and not allowed (client) then
        client:update_permissions (changes)
      end
    end
  end
  if new_client and next (all_denies) and not allowed (new_client) then
    new_client:update_permissions (all_denies)
  end
end

-- The object manager enumerates existing nodes during activation as well as
-- future additions. Reconcile periodically too, since a departing proxy may
-- already be unbound when WirePlumber delivers the removal notification.
nodes_om:connect ("object-added", function () reconcile () end)
clients_om:connect ("object-added", function (_, client) reconcile (client) end)
nodes_om:activate ()
clients_om:activate ()
Core.timeout_add (1000, function ()
  reconcile ()
  return true
end)
