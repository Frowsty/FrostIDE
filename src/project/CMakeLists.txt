# SPDX-License-Identifier: Apache-2.0
#
# frost::project (PLAN.md section 3.2: solution/vcxproj readers, cache, CMake, presets, toolchains,
# trust), the W1 part (ADR 0032): static .sln, .slnx, .vcxproj and .vcxproj.filters readers, the
# SolutionModel behind the Solution Explorer, and the trusted-roots store the build gate checks;
# and the CMake part (ADR 0046): the CMakePresets.json reader, CMake profiles and their command
# lines, the File API query and reply model, and the CMake target tree's view model; and the
# directory roles (UX-23) that decide which folders the project tree, search and watcher traverse.
# Qt-free; depends on io (and core through it). pugixml (ADR 0033) and yyjson (ADR 0021) are linked
# PRIVATE, so no layer above project sees their headers; FrostLayers' deferred pass checks the
# $<LINK_ONLY:...> entries a static library propagates.
# TODO(M2): MSBuild evaluation through path (c) (MSBuild.exe -getTargetResult; ProjectHost follows in
# M16, ADR 0029), ProjectModelCache and restricted mode join here.

frost_add_library(project LAYER project
    SOURCES
        src/cmake_compile_database.cpp
        src/cmake_file_api.cpp
        src/cmake_membership.cpp
        src/cmake_presets.cpp
        src/cmake_profile.cpp
        src/cmake_syntax.cpp
        src/cmake_view_model.cpp
        src/directory_roles.cpp
        src/json.cpp
        src/paths.cpp
        src/process_environment.cpp
        src/sln_reader.cpp
        src/slnx_reader.cpp
        src/solution_model.cpp
        src/text_util.cpp
        src/trust_store.cpp
        src/vcxproj_reader.cpp
        src/xml.cpp
    DEPS frost::io)
target_link_libraries(frost_project PRIVATE pugixml::pugixml yyjson::yyjson)
