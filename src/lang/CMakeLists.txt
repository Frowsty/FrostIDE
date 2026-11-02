# SPDX-License-Identifier: Apache-2.0
#
# frost::lang (PLAN.md section 3.2: LSP client, diagnostics store, ...), the W2 part (ADR 0036):
# JSON-RPC 2.0 with LSP framing, the LSP 3.17 client, clangd discovery and supervision, compile
# commands (an existing compile_commands.json, or one synthesized for a Visual Studio solution),
# document synchronisation from frost::text ChangeSets and the live diagnostics model. Qt-free;
# depends on project, settings and text (and io, sched and core through them). yyjson (MIT, ADR
# 0021) is linked PRIVATE, so no layer above lang sees yyjson.h.
# TODO(M3): the inspection registry, SARIF, clang-tidy tiers and the CMake language service join here.

# The LLVM version pinned in sdk/sdk.lock.json, so findClangd can name the SDK's clangd.
file(READ "${FROST_SOURCE_DIR}/sdk/sdk.lock.json" _frost_sdk_lock)
if(WIN32)
    string(JSON _frost_llvm_version GET "${_frost_sdk_lock}" windows llvm version)
else()
    string(JSON _frost_llvm_version GET "${_frost_sdk_lock}" linux llvm version)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FROST_SOURCE_DIR}/sdk/sdk.lock.json")

frost_add_library(lang LAYER lang
    SOURCES
        src/clangd_locator.cpp
        src/clangd_service.cpp
        src/compile_commands.cpp
        src/diagnostics.cpp
        src/document_sync.cpp
        src/json.cpp
        src/jsonrpc_connection.cpp
        src/jsonrpc_framing.cpp
        src/lsp_client.cpp
        src/msvc_environment.cpp
        src/uri.cpp
    DEPS frost::project frost::settings frost::text frost::io frost::sched frost::core)
target_link_libraries(frost_lang PRIVATE yyjson::yyjson)
target_compile_definitions(frost_lang PRIVATE FROST_PINNED_LLVM_VERSION="${_frost_llvm_version}")
if(WIN32)
    # advapi32: RegGetValueW (the Windows SDK root); shell32: SHGetFolderPathW (vswhere's folder).
    target_link_libraries(frost_lang PRIVATE advapi32 shell32)
endif()
