# SPDX-License-Identifier: Apache-2.0
#
# frost::engine (FEC-1, PLAN.md Appendix C R4): the Qt-free IDE side of the FrostEngine contract.
# It runs the engine's own CLI (FrostEditor <command>, one JSON object per run) through
# io::IProcessLauncher, carries the engine's 64-bit handles exactly (frost::rpc::Json keeps
# integers above 2^53), and reads and validates the contract v2 schema document the engine's
# schema.export command prints (FER-1a): components, the Lua API with its C++ source locations and
# cost tags, the Lua aliases, the events and the commands. Source locations resolve against the
# engine checkout the IDE has open. It sits one rank above lang (cmake/FrostLayers.cmake), so a
# host provider here can later feed the Lua service without the Lua module knowing FrostEngine
# (LUA-13). The live editor session (FER-2) will build on this module.

frost_add_library(engine LAYER engine
    SOURCES
        src/binding_index.cpp
        src/binding_preview.cpp
        src/binding_queries.cpp
        src/command_policy.cpp
        src/crash_artifact.cpp
        src/crash_artifact_source.cpp
        src/engine_cli.cpp
        src/engine_lua.cpp
        src/engine_test_parsers.cpp
        src/engine_test_provider.cpp
        src/handle.cpp
        src/project_content.cpp
        src/schema.cpp
        src/session.cpp
    DEPS frost::lang frost::text frost::rpc frost::io frost::core
    THIRD_PARTY unofficial-sodium::sodium)
