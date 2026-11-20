-- SPDX-License-Identifier: Apache-2.0
-- Real LuaJIT FFI fixture; never run through the Lua 5.4 debugger profile.
local ffi = require('ffi')
local value = ffi.new('int64_t', 42)
print(tostring(value))
