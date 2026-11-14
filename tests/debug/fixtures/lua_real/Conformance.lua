-- SPDX-License-Identifier: Apache-2.0
-- Real R7 debugger workload. Marker lines are discovered from these bytes, never supplied frames.
local captured = 17
local Probe = {}

local function Leaf(payload)
    local speed = payload.speed -- R7_LEAF_ENTRY
    local dt = payload.dt
    local inventory = { label = "r7-inventory", nested = { answer = 42 }, values = { 3, 5, 8 } }
    local cycle = {}
    cycle.self = cycle
    local result = speed * dt + captured -- R7_LOCALS
    result = result + 1 -- R7_STEP_OVER
    return result, inventory, cycle -- R7_LEAF_RETURN
end

local function Middle(payload)
    local result, inventory, cycle = Leaf(payload) -- R7_STEP_IN
    return result, inventory, cycle -- R7_STEP_OUT
end

function Probe:OnCreate()
    self.ticks = 0 -- R7_ON_CREATE
    self.spawn = false
    self.fail = false
    self.child = nil
end

function Probe:OnUpdate()
    self.ticks = self.ticks + 1 -- R7_ENTRY
    local payload = { speed = 10, dt = 0.25, tick = self.ticks }
    local result, inventory, cycle = Middle(payload) -- R7_CALL_MIDDLE
    self.latest = result -- R7_AFTER_CALL
    self.inventory = inventory
    self.cycle = cycle
    if self.spawn and self.child == nil then
        self.child = coroutine.create(function()
            local childValue = 23
            childValue = childValue + captured -- R7_CHILD_COROUTINE
            coroutine.yield(childValue)
            return childValue + 1
        end)
        local ok, value = coroutine.resume(self.child)
        assert(ok, value)
        self.childResult = value
    end
    if self.fail then
        error("R7_REAL_EXCEPTION") -- R7_EXCEPTION
    end
end

return Probe
