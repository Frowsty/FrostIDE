-- SPDX-License-Identifier: Apache-2.0
-- Real LuaUnit 3.5 driver. Discovery must not execute any test body.
local lu = require('luaunit')
TestSelection = {}
function TestSelection:testPass()
    local marker = os.getenv('FROST_LUA_TEST_BODY_MARKER')
    if marker then local file = assert(io.open(marker, 'wb')); assert(file:write('ran')); assert(file:close()) end
    local value = 42 -- LUA_FRAMEWORK_BREAKPOINT
    lu.assertEquals(value, 42)
end
function TestSelection:testFailure() lu.assertEquals(1, 2) end
function TestSelection:testSkip() lu.skip('genuine LuaUnit skip') end
TestSelection['testLiteral[x]%-?'] = function() lu.assertTrue(true) end
os.exit(lu.LuaUnit.run())
