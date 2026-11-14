# SPDX-License-Identifier: Apache-2.0
add_library(frost-local::frost-local INTERFACE IMPORTED)
set_target_properties(frost-local::frost-local PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../include")
