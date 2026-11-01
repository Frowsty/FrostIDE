# SPDX-License-Identifier: Apache-2.0
#
# frost-themec, the build-time theme compiler (ADR 0015 decision 3). It links only
# frost::ui_theme_compiler (and so ui_model and Qt Core): edits to frost::ui never rebuild it,
# and it rewrites a blob only when the bytes change. # Budget: B28.
frost_add_executable(frost-themec LAYER ui
    SOURCES main.cpp
    DEPS frost::ui_theme_compiler
    THIRD_PARTY Qt6::Core)
target_include_directories(frost-themec PRIVATE ${FROST_SOURCE_DIR}/tools/common)
