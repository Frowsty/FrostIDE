# SPDX-License-Identifier: Apache-2.0
#
# frost::ui_theme_compiler (ADR 0015 decision 3): .theme.json to ThemeData and the theme blob,
# on QJsonDocument (Qt Core only) plus the Qt-free duplicate-member pre-scan. Used at build time
# by frost-themec and, from M11, in-product for theme import.
frost_add_library(ui_theme_compiler LAYER ui
    SOURCES
        src/json_prescan.cpp
        src/theme_compiler.cpp
    DEPS frost::ui_model
    THIRD_PARTY Qt6::Core)
