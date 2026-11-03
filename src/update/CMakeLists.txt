# SPDX-License-Identifier: Apache-2.0
# Qt-free authenticated updater primitives.
frost_add_library(update LAYER update
    SOURCES src/verification.cpp src/manifest.cpp src/parent_process.cpp src/payload_guard.cpp
    DEPS frost::core
    THIRD_PARTY unofficial-sodium::sodium yyjson::yyjson)
