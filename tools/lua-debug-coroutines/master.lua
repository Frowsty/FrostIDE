-- SPDX-License-Identifier: Apache-2.0
-- Associate VM-local lifetime IDs with the provider's ordinary DAP thread IDs.
local m = {}
local owner, threads, active = {}, {}, {}

function m.route(threadId, message)
    local child = owner[threadId]
    if child then
        message.frostCoroutineId = child.native
        return child.worker
    end
    return threadId
end

function m.has(threadId)
    return owner[threadId] ~= nil
end

function m.acceptsStep(threadId)
    local worker = owner[threadId] and owner[threadId].worker or threadId
    return not active[worker] or active[worker] == threadId
end

function m.noteStop(worker, native)
    if native == nil then
        return worker
    end
    local id = native == 0 and worker or threads[worker] and threads[worker][native]
    active[worker] = assert(id)
    return id
end

function m.consume(worker, events, allocate, start, exit)
    local live = threads[worker] or {}
    for token in events:gmatch("(-?%d+),") do
        local native = assert(math.tointeger(tonumber(token)))
        if native > 0 and not live[native] then
            local id = allocate()
            live[native] = id
            owner[id] = {worker = worker, native = native}
            start(id)
        elseif native < 0 then
            local id = live[-native]
            if id then
                owner[id], live[-native] = nil, nil
                exit(id)
            end
        end
    end
    threads[worker] = live
end

function m.clear(worker, exit)
    for _, id in pairs(threads[worker] or {}) do
        owner[id] = nil
        exit(id)
    end
    threads[worker] = nil
    active[worker] = nil
end

return m
