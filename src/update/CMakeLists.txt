# SPDX-License-Identifier: Apache-2.0
# Qt-free authenticated updater primitives, HTTP client, network policy, and safe updater.
set(FROST_UPDATE_SOURCES
    src/verification.cpp
    src/manifest.cpp
    src/parent_process.cpp
    src/payload_guard.cpp
    src/network_policy.cpp
    src/safe_updater.cpp
)
if(WIN32)
    list(APPEND FROST_UPDATE_SOURCES src/http_client_win.cpp)
else()
    find_package(CURL REQUIRED GLOBAL)
    list(APPEND FROST_UPDATE_SOURCES src/http_client_curl.cpp)
endif()

frost_add_library(update LAYER update
    SOURCES ${FROST_UPDATE_SOURCES}
    DEPS frost::core frost::io
    THIRD_PARTY unofficial-sodium::sodium yyjson::yyjson)

if(WIN32)
    target_link_libraries(frost_update PRIVATE winhttp)
else()
    target_link_libraries(frost_update PRIVATE CURL::libcurl)
endif()
