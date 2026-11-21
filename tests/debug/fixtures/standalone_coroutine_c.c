// SPDX-License-Identifier: Apache-2.0
// Actual C API coroutine creation for the standalone debugger acceptance test.
#include "lauxlib.h"
#include "lua.h"

static int create(lua_State* state) {
    luaL_checktype(state, 1, LUA_TFUNCTION);
    lua_State* child = lua_newthread(state);
    lua_pushvalue(state, 1);
    lua_xmove(state, child, 1);
    return 1;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int luaopen_frost_test_coroutine(lua_State* state) {
    lua_pushcfunction(state, create);
    return 1;
}
