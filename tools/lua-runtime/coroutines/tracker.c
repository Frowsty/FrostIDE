// SPDX-License-Identifier: Apache-2.0
// The tracker owns metadata only. The VM remains the sole owner of coroutine objects.
#include "tracker.h"

#include "lapi.h"
#include "lauxlib.h"
#include "lstate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THREADS 4096
#define MAX_EVENTS 16384

typedef struct Entry {
    lua_State* thread;
    lua_Integer id;
    int live;
} Entry;

typedef struct Event {
    lua_Integer id;
    int started;
} Event;

typedef struct Tracker {
    Entry threads[MAX_THREADS];
    Event events[MAX_EVENTS];
    size_t occupied;
    size_t eventCount;
    lua_Integer lastId;
    int inspectionReference;
    int failed;
} Tracker;

static Tracker* tracker(lua_State* thread) { return G(thread)->frostCoroutineTracker; }

static Entry* findByThread(Tracker* s, lua_State* thread) {
    for (size_t i = 0; i < s->occupied; ++i)
        if (s->threads[i].thread == thread)
            return &s->threads[i];
    return NULL;
}

static void event(Tracker* s, Entry* entry, int started) {
    entry->live = started;
    if (s->eventCount == MAX_EVENTS) {
        s->failed = 1;
        return;
    }
    s->events[s->eventCount++] = (Event){entry->id, started};
}

void frost_coroutine_created(lua_State* thread) {
    Tracker* s = tracker(thread);
    if (!s || s->failed)
        return;
    if (s->lastId == LUA_MAXINTEGER) {
        s->failed = 1;
        return;
    }
    size_t slot = 0;
    while (slot < s->occupied && s->threads[slot].thread)
        ++slot;
    if (slot == MAX_THREADS) {
        s->failed = 1;
        return;
    }
    if (slot == s->occupied)
        ++s->occupied;
    Entry* entry = &s->threads[slot];
    *entry = (Entry){thread, ++s->lastId, 1};
    event(s, entry, 1);
}

void frost_coroutine_resumed(lua_State* thread) {
    Tracker* s = tracker(thread);
    if (s && !s->failed) {
        Entry* entry = findByThread(s, thread);
        if (entry && !entry->live)
            event(s, entry, 1);
    }
}

void frost_coroutine_exited(lua_State* thread) {
    Tracker* s = tracker(thread);
    if (s && !s->failed) {
        Entry* entry = findByThread(s, thread);
        if (entry && entry->live)
            event(s, entry, 0);
    }
}

void frost_coroutine_collected(lua_State* thread) {
    Tracker* s = tracker(thread);
    if (s && !s->failed) {
        Entry* entry = findByThread(s, thread);
        if (entry) {
            if (entry->live)
                event(s, entry, 0);
            entry->thread = NULL;
            while (s->occupied && !s->threads[s->occupied - 1].thread)
                --s->occupied;
        }
    }
}

void frost_coroutines_close(lua_State* thread) {
    free(tracker(thread));
    G(thread)->frostCoroutineTracker = NULL;
}

static int pinThread(lua_State* base, Tracker* s, lua_State* thread) {
    // The base stack has room before any pointer is resolved. Push onto that
    // stack without allocating, so Lua cannot collect the thread before the
    // registry receives a reference. The visitor releases it after each call.
    setthvalue2s(base, base->top.p, thread);
    api_incr_top(base);
    s->inspectionReference = luaL_ref(base, LUA_REGISTRYINDEX);
    lua_pushlightuserdata(base, thread);
    return 1;
}

// Returns only scalars consumed by Lua Debug's existing host visitor. ID zero
// refers to the actual main state, and a failed tracker never returns a pointer.
static int bridge(lua_State* L) {
    const char* command = luaL_checkstring(L, 1);
    Tracker* s = tracker(L);
    if (strcmp(command, "enable") == 0) {
        if (!s)
            G(L)->frostCoroutineTracker = s = calloc(1, sizeof(Tracker));
        lua_pushboolean(L, s != NULL);
        return 1;
    }
    if (strcmp(command, "release") == 0) {
        if (s && s->inspectionReference > 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, s->inspectionReference);
            s->inspectionReference = 0;
        }
        return 0;
    }
    if (!s || s->failed)
        return luaL_error(L, "Coroutine tracker capacity exceeded or initialization failed");
    if (strcmp(command, "active") == 0) {
        if (L == G(L)->mainthread) {
            lua_pushinteger(L, 0);
            return 1;
        }
        Entry* entry = findByThread(s, L);
        if (!entry || !entry->live)
            return luaL_error(L, "The running Lua coroutine has no tracked identity");
        lua_pushinteger(L, entry->id);
        return 1;
    }
    if (strcmp(command, "lookup") == 0) {
        const lua_Integer id = luaL_checkinteger(L, 2);
        if (s->inspectionReference > 0)
            return luaL_error(L, "The previous Lua coroutine inspection was not released");
        luaL_checkstack(L, 2, "coroutine inspection");
        if (id == 0) {
            lua_pushlightuserdata(L, G(L)->mainthread);
            return 1;
        }
        for (size_t i = 0; i < s->occupied; ++i)
            if (s->threads[i].thread && s->threads[i].id == id && s->threads[i].live) {
                return pinThread(L, s, s->threads[i].thread);
            }
        return 0;
    }
    if (strcmp(command, "events") == 0) {
        if (s->eventCount == 0) {
            lua_pushliteral(L, "");
            return 1;
        }
        luaL_Buffer buffer;
        luaL_buffinit(L, &buffer);
        for (size_t i = 0; i < s->eventCount; ++i) {
            char token[48];
            const lua_Integer id = s->events[i].id;
            const int n = snprintf(token, sizeof(token), LUA_INTEGER_FMT ",", s->events[i].started ? id : -id);
            luaL_addlstring(&buffer, token, (size_t)n);
        }
        luaL_pushresult(&buffer);
        if (s->failed)
            return luaL_error(L, "Coroutine lifecycle journal exceeded its capacity");
        s->eventCount = 0;
        return 1;
    }
    return luaL_error(L, "Unknown coroutine debugger operation");
}

int luaopen_frost_coroutine_debugger(lua_State* L) {
    lua_pushcfunction(L, bridge);
    return 1;
}
