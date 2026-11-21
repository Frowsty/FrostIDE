-- SPDX-License-Identifier: Apache-2.0
local captured = 17
local function add(a, b)
    return a + b -- STANDALONE_STEP_IN
end
local function leaf(speed)
    local inventory = { nested = { answer = 42 } }
    local result = add(speed, captured) -- STANDALONE_LOCALS
    result = result + 1 -- STANDALONE_STEP_OVER
    return result -- STANDALONE_LEAF_RETURN
end
package.loaded["frost.coroutine_debugger.v1"] = function() return nil end
local first = leaf(25)
local side = coroutine.create(function()
    local sideValue = 73
    coroutine.yield() -- STANDALONE_SUSPENDED
    return sideValue
end)
assert(coroutine.resume(side))
local create = coroutine.create
local nativeModule = os.getenv("FROST_LUA_COROUTINE_TEST_MODULE")
if nativeModule then
    create = assert(package.loadlib(nativeModule, "luaopen_frost_test_coroutine"))()
end
local co = create(function()
    local childValue = 23
    childValue = childValue + 1 -- STANDALONE_COROUTINE
    return childValue
end)
local ok, answer = coroutine.resume(co)
local expected = 24 -- STANDALONE_AFTER_COMPLETION
assert(ok and answer == 24)
side = nil
collectgarbage("collect")
assert(answer == expected) -- STANDALONE_AFTER_COLLECTION
assert(_VERSION == "Lua 5.4")
assert(arg[1] == "two words" and arg[2] == "")
print("STANDALONE_RESULT", first, _VERSION, arg[1], #arg[2])
