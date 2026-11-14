# SPDX-License-Identifier: Apache-2.0
# Standalone Lua 5.4 from a previously digest-verified source tree, not an embedded engine VM.
cmake_minimum_required(VERSION 3.28)
project(FrostStandaloneLua VERSION 5.4.9 LANGUAGES C)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Standalone Lua must use the pinned Clang compiler")
endif()
if(NOT DEFINED FROST_PINNED_CLANG_VERSION OR NOT CMAKE_C_COMPILER_VERSION VERSION_EQUAL FROST_PINNED_CLANG_VERSION)
    message(FATAL_ERROR "Pass the sdk.lock pinned Clang version; the actual compiler must match it")
endif()
if(NOT IS_DIRECTORY "${LUA_SOURCE_DIR}/src")
    message(FATAL_ERROR "Pass the verified Lua source directory")
endif()
set(_lua_core lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c
    lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c
    lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c lmathlib.c loadlib.c loslib.c
    lstrlib.c ltablib.c lutf8lib.c linit.c)
list(TRANSFORM _lua_core PREPEND "${LUA_SOURCE_DIR}/src/")
add_library(lua54 SHARED ${_lua_core})
target_include_directories(lua54 PUBLIC "${LUA_SOURCE_DIR}/src")
set_target_properties(lua54 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
if(WIN32)
    # Core/library C files export LUA_API; lua.c imports that exact external Lua 5.4 ABI.
    # A static interpreter would not supply lua-debug's external DLL/API loading contract.
    target_compile_definitions(lua54 PUBLIC LUA_BUILD_AS_DLL)
    set_property(TARGET lua54 PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
else()
    target_compile_definitions(lua54 PRIVATE LUA_USE_LINUX)
    target_link_libraries(lua54 PRIVATE m dl)
endif()
add_executable(lua "${LUA_SOURCE_DIR}/src/lua.c")
target_link_libraries(lua PRIVATE lua54)
set_target_properties(lua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
if(WIN32)
    set_property(TARGET lua PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
    enable_language(RC)
    configure_file(lua.rc.in "${CMAKE_CURRENT_BINARY_DIR}/lua.rc" @ONLY)
    target_sources(lua PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/lua.rc")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/lua.rc" PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lua.manifest")
    target_link_options(lua PRIVATE /MANIFEST:NO)
else()
    target_link_options(lua PRIVATE "-Wl,-E")
    set_target_properties(lua PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
install(TARGETS lua lua54 RUNTIME DESTINATION bin LIBRARY DESTINATION bin ARCHIVE DESTINATION lib)
install(FILES "${LUA_SOURCE_DIR}/src/lua.h" "${LUA_SOURCE_DIR}/src/luaconf.h"
    "${LUA_SOURCE_DIR}/src/lauxlib.h" "${LUA_SOURCE_DIR}/src/lualib.h" DESTINATION include)
# lua.h carries the source distribution's full copyright/MIT permission notice.
install(FILES "${LUA_SOURCE_DIR}/src/lua.h" "${LUA_SOURCE_DIR}/doc/readme.html" DESTINATION share/licenses/Lua)
