# SPDX-License-Identifier: Apache-2.0
#
# frost::rpc (PLAN.md section 3.2: "transports and framing"; ADR 0050): the pieces the protocol
# clients share, Qt-free and below them in the layering: frost::rpc::Json (the owning JSON value,
# parsed with yyjson) and the Content-Length frame codec of LSP's and DAP's base protocol. Both moved
# here from frost::lang, which re-exports them, so frost::debug's DAP client can use them without
# depending on the LSP layer of the same rank. yyjson (MIT, ADR 0021) is linked PRIVATE, so no layer
# above sees yyjson.h.

frost_add_library(rpc LAYER rpc
    SOURCES
        src/framing.cpp
        src/json.cpp
    DEPS frost::core)
target_link_libraries(frost_rpc PRIVATE yyjson::yyjson)
