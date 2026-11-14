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
local first = leaf(25)
local co = coroutine.create(function()
    local childValue = 23
    childValue = childValue + 1 -- STANDALONE_COROUTINE
    return childValue
end)
local ok, answer = coroutine.resume(co)
assert(ok and answer == 24)
assert(_VERSION == "Lua 5.4")
assert(arg[1] == "two words" and arg[2] == "")
print("STANDALONE_RESULT", first, _VERSION, arg[1], #arg[2])
