# SPDX-License-Identifier: Apache-2.0
#
# Fixture for tests/cmake/check_layering.cmake: a project whose targets break the
# layering rules in every way FrostLayers.cmake must catch, plus a valid part
# (targets named ok_*) that must produce no diagnostic. Configured with
# FROST_LAYERS_REPORT_ALL so every violation is reported in one run.
cmake_minimum_required(VERSION 4.0)
project(frost_layering_fixture LANGUAGES CXX)
set(FROST_LAYERS_REPORT_ALL ON)
list(APPEND CMAKE_MODULE_PATH "${FROST_SOURCE_DIR}/cmake")
include(FrostLayers)

# A stand-in for Qt: only its name matters to the checks.
add_library(Qt6::Core INTERFACE IMPORTED)

# Valid: io may use core and compat; ui may use Qt.
frost_add_library(ok_compat LAYER compat INTERFACE)
frost_add_library(ok_core LAYER core INTERFACE DEPS frost::ok_compat)
frost_add_library(ok_io LAYER io INTERFACE DEPS frost::ok_core)
frost_add_library(ok_ui LAYER ui INTERFACE DEPS frost::ok_io THIRD_PARTY Qt6::Core)

# 1. A declared upward dependency.
frost_add_library(bad_upward LAYER core INTERFACE DEPS frost::ok_io)
# 2. Qt declared outside ui/app.
frost_add_library(bad_qt LAYER io INTERFACE THIRD_PARTY Qt6::Core)
# 3. A FrostIDE library smuggled in as THIRD_PARTY.
frost_add_library(bad_smuggle LAYER core INTERFACE THIRD_PARTY frost::ok_io)
# 4. Qt reached through a wrapper target.
add_library(qt_wrapper INTERFACE)
target_link_libraries(qt_wrapper INTERFACE Qt6::Core)
frost_add_library(bad_wrapper LAYER io INTERFACE THIRD_PARTY qt_wrapper)
# 5. An upward link added afterwards with a raw target_link_libraries.
frost_add_library(bad_raw LAYER core INTERFACE)
target_link_libraries(frost_bad_raw INTERFACE frost::ok_io)
