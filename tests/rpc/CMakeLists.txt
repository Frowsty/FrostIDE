# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::rpc (ADR 0050), the Content-Length codec and the JSON value the LSP and DAP
# clients share:
#   rpc_tests  encodeFrame and FrameDecoder (split, joined and malformed input, oversized frames),
#              parseJson and dump round trips, and one decoder per thread; label "unit". These run
#              under TSan in the WSL quick leg (tools/verify_support.py, module rpc). The LSP's
#              framing and JSON-RPC tests stay in tests/lang, the DAP client's in tests/debug.
frost_add_test(rpc_tests
    SOURCES
        rpc_test.cpp
    DEPS frost::rpc frost::core
    LABELS unit)
