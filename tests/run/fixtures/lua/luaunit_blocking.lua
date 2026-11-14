-- SPDX-License-Identifier: Apache-2.0
-- Actual running LuaUnit child cancellation, not a fake process verdict.
local lu = require('luaunit')
function testBlocks()
    io.write('LUA_TEST_BODY_RUNNING\n'); io.flush()
    while true do end
end
os.exit(lu.LuaUnit.run())
