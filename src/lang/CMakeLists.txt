# SPDX-License-Identifier: Apache-2.0
#
# frost::lang (PLAN.md section 3.2: LSP client, diagnostics store, ...), the W2 part (ADR 0036):
# JSON-RPC 2.0 with LSP framing, the LSP 3.17 client, clangd discovery and supervision, compile
# commands (an existing compile_commands.json, or one synthesized for a Visual Studio solution),
# document synchronisation from frost::text ChangeSets and the live diagnostics model; and (ADR 0041)
# compile commands from MSBuild's evaluation, completion, signature help, hover, navigation, usages,
# symbols, rename, code actions, formatting and workspace edits as Qt-free models; and (ADR 0047)
# semantic tokens (full and delta) and clangd's inactive regions. Qt-free;
# depends on project, settings and text (and io, sched and core through them). yyjson (MIT, ADR
# 0021) is linked PRIVATE, so no layer above lang sees yyjson.h.
# TODO(M3): the inspection registry, SARIF, clang-tidy tiers and the CMake language service join here.

# The LLVM and LuaLS versions pinned in sdk/sdk.lock.json, so findClangd and defaultLuaSearch can
# name the SDK's and the staged folder's clangd and lua-language-server (ADR 0079).
file(READ "${FROST_SOURCE_DIR}/sdk/sdk.lock.json" _frost_sdk_lock)
if(WIN32)
    string(JSON _frost_llvm_version GET "${_frost_sdk_lock}" windows llvm version)
    string(JSON _frost_luals_version GET "${_frost_sdk_lock}" windows luals version)
else()
    string(JSON _frost_llvm_version GET "${_frost_sdk_lock}" linux llvm version)
    string(JSON _frost_luals_version GET "${_frost_sdk_lock}" linux luals version)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FROST_SOURCE_DIR}/sdk/sdk.lock.json")

frost_add_library(lang LAYER lang
    SOURCES
        src/clangd_locator.cpp
        src/clangd_service.cpp
        src/code_assist.cpp
        src/compile_commands.cpp
        src/completion.cpp
        src/diagnostics.cpp
        src/document_sync.cpp
        src/engine_automation.cpp
        src/engine_discovery_process.cpp
        src/fuzzy.cpp
        src/jsonrpc_connection.cpp
        src/jsonrpc_framing.cpp
        src/lsp_client.cpp
        src/lua_config.cpp
        src/lua_service.cpp
        src/managed_service.cpp
        src/lsp_types.cpp
        src/msbuild_compile_commands.cpp
        src/msvc_environment.cpp
        src/navigation.cpp
        src/refactoring.cpp
        src/semantic_tokens.cpp
        src/snippet.cpp
        src/symbols.cpp
        src/uri.cpp
        src/workspace_edit.cpp
    DEPS frost::project frost::settings frost::text frost::rpc frost::io frost::sched frost::core)
target_compile_definitions(frost_lang PRIVATE FROST_PINNED_LLVM_VERSION="${_frost_llvm_version}"
    FROST_PINNED_LUALS_VERSION="${_frost_luals_version}")
if(WIN32)
    # advapi32: RegGetValueW (the Windows SDK root); shell32: SHGetFolderPathW (vswhere's folder).
    target_link_libraries(frost_lang PRIVATE advapi32 shell32)
endif()
