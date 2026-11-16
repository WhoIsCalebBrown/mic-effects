-- Regression coverage for the WirePlumber microphone-visibility script.
-- Run from the repository root: lua tests/tst_hide_mics.lua

local function fail (message)
  error ("tst_hide_mics: " .. message, 2)
end

local function assert_equal (actual, expected, message)
  if actual ~= expected then
    fail ((message or "values differ") .. ": got " .. tostring (actual)
      .. ", expected " .. tostring (expected))
  end
end

local function copy (table_)
  local result = {}
  for key, value in pairs (table_) do result [key] = value end
  return result
end

local clients = {}
local nodes = {
  {
    properties = {
      ["node.name"] = "camera-effects-mic",
      ["object.serial"] = "100",
      ["media.class"] = "Audio/Source/Virtual",
    },
    ["bound-id"] = 42,
  },
  {
    properties = {
      ["node.name"] = "mic-effects",
      ["media.class"] = "Audio/Source/Virtual",
    },
    ["bound-id"] = 43,
  },
}

local function client (properties)
  local result = { properties = properties, updates = {} }
  function result:update_permissions (permissions)
    self.updates [#self.updates + 1] = copy (permissions)
  end
  return result
end

local app = client {}
local effects = client { ["mic-effects.client"] = "true" }
clients = { app, effects }

Log = { open_topic = function () return { info = function () end } end }
Json = { Object = function (value) return value end }
Conf = {
  get_section_as_json = function ()
    return { parse = function () return { all = true } end }
  end,
}
Interest = function (spec) return spec end
Constraint = function (spec) return spec end
EventInterest = function (spec) return spec end

local managers = {}
ObjectManager = function (spec)
  local kind = spec [1].type
  local manager = { kind = kind, items = kind == "client" and clients or nodes }
  function manager:connect (signal, callback) self [signal] = callback end
  function manager:iterate ()
    local index = 0
    return function ()
      index = index + 1
      return self.items [index]
    end
  end
  function manager:lookup (interest)
    for _, node in ipairs (self.items) do
      if node.properties ["node.name"] == interest [1] [3] then return node end
    end
  end
  function manager:activate ()
    self.activated = true
    if self ["object-added"] then
      for _, object in ipairs (self.items) do self ["object-added"] (self, object) end
    end
  end
  managers [kind] = manager
  return manager
end

local timer
Core = { timeout_add = function (_, callback) timer = callback end }
SimpleEventHook = function () return { register = function () end } end

assert (loadfile ("wireplumber/mic-effects-hide-mics.lua")) ()

assert_equal (app.updates [#app.updates] [42], "-",
  "pre-existing virtual source is hidden once mic-effects exists")
assert_equal (#effects.updates, 0, "mic-effects client remains exempt")

local virtual = nodes [2]
local function replace (serial, name, class, id)
  nodes [1] = {
    properties = { ["object.serial"] = serial, ["node.name"] = name, ["media.class"] = class },
    ["bound-id"] = id or 42,
  }
end

-- PipeWire clears permissions on destruction. The same mic can reconnect
-- with the same id and name before the next check, but its serial changes.
local before = #app.updates
replace ("101", "camera-effects-mic", "Audio/Source/Virtual")
timer ()
assert_equal (#app.updates, before + 1, "reconnected microphone receives a fresh deny")
assert_equal (app.updates [#app.updates] [42], "-", "same-name replacement is hidden")
before = #app.updates
timer ()
assert_equal (#app.updates, before, "stable graph does not rewrite permissions")

-- A late client gets the current bans even when the graph did not change.
local late = client {}
table.insert (clients, late)
managers.client ["object-added"] (managers.client, late)
assert_equal (late.updates [1] [42], "-", "late client receives current deny")

-- Losing the virtual mic restores the microphone which was actually hidden.
table.remove (nodes, 2)
timer ()
assert_equal (app.updates [#app.updates] [42], "rwxm", "original source is restored")
nodes [2] = virtual
timer ()
assert_equal (app.updates [#app.updates] [42], "-", "returning virtual mic reinstates deny")

-- A restricted unrelated object reuses the old global id and the virtual
-- mic disappears before either change is polled. Neither a client connecting
-- in this interval nor the next timer may grant permissions for that id.
before = #app.updates
local restricted = client {}
table.insert (clients, restricted)
table.remove (nodes, 2)
replace ("102", "restricted-video", "Video/Source")
managers.client ["object-added"] (managers.client, restricted)
timer ()
assert_equal (#app.updates, before, "reused id receives no permission update")
assert_equal (#restricted.updates, 0, "new client retains restricted permissions")

nodes [2] = virtual
replace ("103", "unbound-mic", "Audio/Source", 0xffffffff)
timer ()
assert_equal (#app.updates, before, "unbound node cannot set wildcard permissions")
assert_equal (#effects.updates, 0, "daemon remains exempt throughout")

print ("tst_hide_mics: ok")
