# SPDX-License-Identifier: Apache-2.0
# Offline protocol/auth policy fixtures only; no real CLI or provider is started.
frost_add_test(agent_tests SOURCES inspection_info_test.cpp protocol_test.cpp DEPS frost::agent frost::io frost::rpc)
