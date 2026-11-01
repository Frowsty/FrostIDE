# SPDX-License-Identifier: Apache-2.0
#
# Fixture for tests/cmake/check_compiler_cache.cmake: a project without languages that
# only includes a copy of FrostCompilerCache.cmake. The copy sits in
# ${FROST_FIXTURE_REPO}/cmake, so the module reads the test's own
# ${FROST_FIXTURE_REPO}/sdk/sdk.lock.json, not the repository's lock. The last line
# reports the launcher the module chose.
cmake_minimum_required(VERSION 4.0)
project(frost_compiler_cache_fixture LANGUAGES NONE)
include("${FROST_FIXTURE_REPO}/cmake/FrostCompilerCache.cmake")
message(STATUS "fixture: launcher '${CMAKE_CXX_COMPILER_LAUNCHER}'")
