-- SPDX-License-Identifier: Apache-2.0
-- Frozen before measurements: 120000 iterations, 1000 per chunk, ordinary calls/loops/coroutines.
-- Do not tune these constants or sites after observing overhead. Python independently derives
-- the integer checksum and all five executed-site counts; Lua clock is reported without a CPU claim.
local Iterations = 120000
local ChunkSize = 1000
local Modulus = 104729
local Probe = {}

local function Fold(first, last, seed, sites)
    sites[2] = sites[2] + 1 -- R7_PERF_SITE_2
    local value = seed
    local nativeSum = 0
    for i = first, last do
        value = (value * 33 + i) % Modulus
        nativeSum = nativeSum + math.sin(i * 0.001) * math.cos(i * 0.002) + math.sqrt(i)
    end
    sites[3] = sites[3] + 1 -- R7_PERF_SITE_3
    return value, nativeSum
end

function Probe:OnCreate()
    self.completed = false
    self.armed = false
end

function Probe:OnUpdate()
    if not self.armed then return end -- R7_PERF_GATE
    if self.completed then return end
    self.completed = true
    local sites = { 0, 0, 0, 0, 0 }
    local started = os.clock()
    local value = 7
    local nativeSum = 0
    sites[1] = sites[1] + 1 -- R7_PERF_SITE_1
    for first = 1, Iterations, ChunkSize do
        local worker = coroutine.create(function()
            local folded, native = Fold(first, math.min(first + ChunkSize - 1, Iterations), value, sites)
            sites[4] = sites[4] + 1 -- R7_PERF_SITE_4
            return folded, native
        end)
        local ok, folded, native = coroutine.resume(worker)
        assert(ok, folded)
        value = folded
        nativeSum = nativeSum + native
    end
    sites[5] = sites[5] + 1 -- R7_PERF_SITE_5
    local elapsed = os.clock() - started
    print(string.format("R7_PERF_RESULT checksum=%d iterations=%d sites=%d,%d,%d,%d,%d luaClockSeconds=%.9f nativeSum=%.9f",
        value, Iterations, sites[1], sites[2], sites[3], sites[4], sites[5], elapsed, nativeSum))
end

return Probe
