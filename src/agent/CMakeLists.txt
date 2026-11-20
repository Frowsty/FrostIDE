# SPDX-License-Identifier: Apache-2.0
# Qt-free consent/native protocol policy. Provider processes are started only by explicit app actions.
frost_add_library(agent LAYER agent
    SOURCES src/hierarchy_edges.cpp src/inspection_info.cpp src/protocol.cpp src/mcp.cpp src/native_session.cpp
    DEPS frost::core frost::io frost::rpc)
