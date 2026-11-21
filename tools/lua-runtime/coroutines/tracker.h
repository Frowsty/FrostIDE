// SPDX-License-Identifier: Apache-2.0
// Private hooks installed only by the opt-in standalone coroutine runtime recipe.
#ifndef FROST_LUA_COROUTINES_H
#define FROST_LUA_COROUTINES_H

#include "lua.h"

void frost_coroutine_created(lua_State* child);
void frost_coroutine_resumed(lua_State* thread);
void frost_coroutine_exited(lua_State* thread);
void frost_coroutine_collected(lua_State* thread);
void frost_coroutines_close(lua_State* thread);
int luaopen_frost_coroutine_debugger(lua_State* thread);

#endif
