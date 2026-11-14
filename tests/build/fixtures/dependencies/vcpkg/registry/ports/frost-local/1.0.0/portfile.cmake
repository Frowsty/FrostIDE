# SPDX-License-Identifier: Apache-2.0
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/frost_local.hpp" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/frost-local-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/frost-local")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/copyright" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
