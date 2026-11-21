# SPDX-License-Identifier: Apache-2.0
# Installs the opt-in VM lifecycle observer into the admitted Lua source tree.
"""Checked edits to the digest-verified Lua 5.4.9 sources in an opt-in runtime build."""
from pathlib import Path


def apply_coroutine_hooks(source: Path) -> None:
    edits = {
        "lstate.h": [
            ("} global_State;", "  void *frostCoroutineTracker;\n} global_State;"),
        ],
        "lstate.c": [
            ('#include "lstate.h"', '#include "lstate.h"\n#include "tracker.h"'),
            ("  g->mainthread = L;", "  g->mainthread = L;\n  g->frostCoroutineTracker = NULL;"),
            ("  stack_init(L1, L);  /* init stack */",
             "  stack_init(L1, L);  /* init stack */\n  frost_coroutine_created(L1);"),
            ("  luai_userstatefree(L, L1);", "  luai_userstatefree(L, L1);\n  frost_coroutine_collected(L1);"),
            ("  status = luaE_resetthread(L, L->status);",
             "  status = luaE_resetthread(L, L->status);\n  frost_coroutine_exited(L);"),
            ("  luaM_freearray(L, G(L)->strt.hash, G(L)->strt.size);",
             "  frost_coroutines_close(L);\n  luaM_freearray(L, G(L)->strt.hash, G(L)->strt.size);"),
        ],
        "ldo.c": [
            ('#include "lstate.h"', '#include "lstate.h"\n#include "tracker.h"'),
            ("  luai_userstateresume(L, nargs);",
             "  frost_coroutine_resumed(L);\n  luai_userstateresume(L, nargs);"),
            ("  *nresults = (status == LUA_YIELD)",
             "  if (status != LUA_YIELD) frost_coroutine_exited(L);\n  *nresults = (status == LUA_YIELD)"),
        ],
        "linit.c": [
            ('#include "lauxlib.h"', '#include "lauxlib.h"\n#include "tracker.h"'),
            ("    lua_pop(L, 1);  /* remove lib */\n  }",
             '    lua_pop(L, 1);  /* remove lib */\n  }\n'
             '  luaL_requiref(L, "frost.coroutine_debugger.v1", luaopen_frost_coroutine_debugger, 0);\n'
             '  lua_pop(L, 1);'),
        ],
    }
    for name, substitutions in edits.items():
        path = source / "src" / name
        original = path.read_text(encoding="utf8")
        for before, after in substitutions:
            if original.count(before) != 1:
                raise ValueError(f"unexpected Lua 5.4.9 source context in {name}")
            original = original.replace(before, after, 1)
        path.write_text(original, encoding="utf8", newline="\n")
