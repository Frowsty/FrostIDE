-- SPDX-License-Identifier: Apache-2.0
-- LUA-12: LuaUnit 3.5 selection-only bridge. Installed before the unchanged user driver.
-- No setup/body/output runner is called. The real framework resolves and expands its inputs.
return function(report)
    local lu = require('luaunit')
    assert(lu.VERSION == '3.5', 'FrostIDE supports LuaUnit 3.5 discovery only')
    local runner = assert(lu.LuaUnit)
    for _, key in ipairs({'internalRunSuiteByInstances', 'applyTestPrefixSuffixFilter',
        'expandClasses', 'applyPatternFilter', 'splitClassMethod', 'asFunction'}) do
        assert(type(runner[key]) == 'function', 'Unsupported LuaUnit discovery API: '..key)
    end
    local exit = os.exit
    local function xml(s)
        assert(type(s)=='string' and not s:find('[%z\1-\8\11\12\14-\31]'), 'Invalid XML discovery text')
        return (s:gsub('&','&amp;'):gsub('<','&lt;'):gsub('>','&gt;'):gsub('"','&quot;'):gsub("'",'&apos;'))
    end
    runner.internalRunSuiteByInstances = function(self, instances)
        local testable = self:applyTestPrefixSuffixFilter(instances)
        local expanded = self:expandClasses(testable)
        local selected = self.applyPatternFilter(self.patternIncludeFilter, expanded)
        local records = {'<?xml version="1.0" encoding="UTF-8"?>',
            '<LuaUnitDiscovery version="3.5" hook="internalRunSuiteByInstances">'}
        local seen = {}
        for _, entry in ipairs(selected) do
            local name, instance = entry[1], entry[2]
            if name ~= 'setupSuite' and name ~= 'teardownSuite' then
                assert(not seen[name], 'Duplicate LuaUnit case: '..name)
                seen[name] = true
                local func = runner.asFunction(instance)
                if not func then
                    local _, method = runner.splitClassMethod(name)
                    func = runner.asFunction(instance[method])
                end
                assert(func, 'Unsupported LuaUnit test function: '..name)
                local info = debug.getinfo(func, 'S')
                local attributes = ' name="'..xml(name)..'"'
                if info and info.source:sub(1,1)=='@' and info.linedefined>0 then
                    attributes=attributes..' file="'..xml(info.source:sub(2))..'" line="'..info.linedefined..'"'
                end
                records[#records+1] = '<TestCase'..attributes..'/>'
            end
        end
        records[#records+1] = '</LuaUnitDiscovery>'
        local file = assert(io.open(report, 'wb'))
        assert(file:write(table.concat(records, '\n')))
        assert(file:close())
        -- Successful catalogue exit, never a test verdict. It also prevents driver code after
        -- runSuite from executing. A driver that never calls this hook writes no catalogue.
        exit(0, true)
    end
end
