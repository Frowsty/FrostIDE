# SPDX-License-Identifier: Apache-2.0
#
# frost::core: error model, contract checks, strong index types, the clock seam,
# thread-safety annotations and annotated locks, the logging facade and build information.
# Qt-free; everything else builds on it.
configure_file(include/frost/core/build_info.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/frost/core/build_info.hpp @ONLY)

frost_add_library(core LAYER core
    SOURCES
        src/assert.cpp
        src/clock.cpp
        src/error.cpp
        src/log.cpp
    DEPS frost::compat)
target_include_directories(frost_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
