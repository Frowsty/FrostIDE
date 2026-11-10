-- SPDX-License-Identifier: Apache-2.0
-- The malicious-repository fixture's script (FND-8): LuaLS would load .luarc.json's plugin, which
-- runs code inside the language server, so no LuaLS may start before the folder is trusted.
local value = undefined_global_of_the_fixture
return value
