-- SPDX-License-Identifier: Apache-2.0
-- Route the stopped VM's independently selectable coroutine frames and values.
local rdebug = require "luadebug.visitor"
local hookmgr = require "luadebug.hookmgr"
local m = {}
local operations = {
    scopes = "frameId", evaluate = "frameId", setExpression = "frameId",
    variables = "valueId", setVariable = "valueId",
    readMemory = "memoryReference", writeMemory = "memoryReference",
}
local frames, values, memoryRefs, memoryIds = {}, {}, {}, {}
local nextReference = 0
local base, native, bridge, enabled, failed, send, pinned

local function invoke(command, id)
    local ok, result = rdebug.eval(bridge, command, id)
    if not ok then
        return nil, result
    end
    return result
end

local function unavailable(message)
    if not failed then
        failed = true
        send("coroutineFailure") {message = message}
    end
end

function m.initialize(push)
    send = push
    if enabled or failed then
        return
    end
    local capture = assert(rdebug.load([[
        local call = package.loaded["frost.coroutine_debugger.v1"]
        if type(call) ~= "function" or debug.getinfo(call, "S").what ~= "C" then
            error("Select the opt-in FrostIDE Lua 5.4.9 coroutine runtime for both Run and Debug")
        end
        package.loaded["frost.coroutine_debugger.v1"] = nil
        return { call = call }
    ]]))
    local captured, environment = rdebug.watch(capture)
    if not captured then
        unavailable(environment)
        return
    end
    -- Keep the small environment and its C function in the provider's persistent
    -- loaded-function reference. Per-stop cleanup may discard the temporary watch.
    bridge = assert(rdebug.load("return call(...)"))
    local _, upvalue = rdebug.getupvalue(bridge, 1)
    if not upvalue or not rdebug.assign(upvalue, environment) then
        unavailable("Unable to bind the Lua coroutine tracker")
        return
    end
    local result, err
    if bridge then
        result, err = invoke("enable")
    end
    enabled = result == true
    if not enabled then
        unavailable(err or "Unable to start the Lua coroutine tracker")
    end
end

local function lookup(id)
    local host, message = invoke("lookup", id)
    if type(host) ~= "userdata" then
        return nil, message
    end
    pinned = id ~= 0
    return host
end

function m.update()
    if not enabled or failed then
        return false
    end
    local events, err = invoke("events")
    if type(events) ~= "string" then
        unavailable(err or "Unable to read Lua coroutine lifecycle events")
        return false
    end
    if events ~= "" then
        send("coroutineEvents") {events = events}
    end
    return true
end

function m.stop(host, reason)
    frames, values, memoryRefs, memoryIds = {}, {}, {}, {}
    base = host
    if not m.update() then
        return false
    end
    local active, err = invoke("active")
    if active == nil then
        unavailable(err or "Unable to identify the stopped Lua coroutine")
        return false
    end
    reason.native = active
    reason.allThreadsStopped = true
    return true
end

function m.resume()
    frames, values, memoryRefs, memoryIds = {}, {}, {}, {}
    base = nil
end

local function fail(pkg, message)
    send(pkg.cmd) {
        command = pkg.command, seq = pkg.seq, success = false, message = message,
    }
end

function m.reportCommandError(pkg, message)
    if enabled and pkg.command and pkg.seq then
        fail(pkg, tostring(message))
    end
end

local function addReference(registry, item, thread)
    if nextReference == 0xFFFFFF then
        error("Lua debugger has exhausted its stopped reference space")
    end
    nextReference = nextReference + 1
    registry[nextReference] = {item = item, thread = thread}
    return nextReference
end

local function wrapValue(body)
    if body.variablesReference and body.variablesReference ~= 0 then
        body.variablesReference = addReference(values, body.variablesReference, native)
    end
    if body.memoryReference then
        local key = native .. ":" .. body.memoryReference
        memoryIds[key] = memoryIds[key] or addReference(memoryRefs, body.memoryReference, native)
        body.memoryReference = tostring(memoryIds[key])
    end
end

function m.wrap(command, response)
    if not enabled or native == nil then
        return true
    end
    if command == "eventMemory" then
        local id = memoryIds[native .. ":" .. response.memoryReference]
        if not id then
            return false
        end
        response.memoryReference = tostring(id)
    end
    if not response.body then
        return true
    end
    local body = response.body
    if command == "scopes" then
        for _, item in ipairs(body.scopes) do wrapValue(item) end
    elseif command == "variables" then
        for _, item in ipairs(body.variables) do wrapValue(item) end
    elseif command == "evaluate" or command == "setExpression" then
        wrapValue(body)
    end
    return true
end

function m.stack(pkg, trace, skipFrame)
    if not base then
        fail(pkg, "Pause the Lua runtime before requesting a coroutine stack")
        return
    end
    hookmgr.sethost(base)
    local id = pkg.frostCoroutineId or 0
    local active = invoke("active")
    local host = lookup(id)
    if not host then
        fail(pkg, "Lua coroutine has already exited")
        return
    end
    local start = math.tointeger(pkg.startFrame or 0)
    local levels = (pkg.levels and pkg.levels ~= 0) and pkg.levels or 200
    levels = math.tointeger(levels)
    if not start or not levels or start < 0 or start > 0xFFFF or levels < 1 then
        fail(pkg, "Invalid Lua coroutine stack page")
        return
    end
    local skip = id == active and skipFrame or 0
    levels = math.min(levels, 512, 0x10000 - start - skip)
    local result = {}
    hookmgr.sethost(host)
    local finished, count = trace(result, 0, start + skip, levels)
    for _, frame in ipairs(result) do
        frame.id = addReference(frames, frame.id, id)
    end
    send("stackTrace") {
        command = pkg.command, seq = pkg.seq, success = true,
        body = {stackFrames = result, totalFrames = finished and (start + count) or nil},
    }
end

function m.dispatch(f, pkg)
    local field = operations[pkg.cmd]
    if not field then
        f(pkg)
        return
    end
    local registry = field == "frameId" and frames or (field == "valueId" and values or memoryRefs)
    local ref = base and registry[tonumber(pkg[field])]
    if not ref then
        fail(pkg, "Lua frame or variable reference is no longer valid")
        return
    end
    hookmgr.sethost(base)
    local host = lookup(ref.thread)
    if not host then
        fail(pkg, "Lua coroutine has already exited")
        return
    end
    pkg[field] = ref.item
    native = ref.thread
    hookmgr.sethost(host)
    f(pkg)
end

function m.restore(host)
    native = nil
    hookmgr.sethost(host)
    if pinned then
        pinned = nil
        invoke("release")
    end
end

function m.enabled()
    return enabled
end

return m
