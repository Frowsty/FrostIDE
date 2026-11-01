# SPDX-License-Identifier: Apache-2.0
#
# frost::project (PLAN.md section 3.2: solution/vcxproj readers, cache, CMake, presets, toolchains,
# trust), the W1 part (ADR 0032): static .sln, .slnx, .vcxproj and .vcxproj.filters readers, the
# SolutionModel behind the Solution Explorer, and the trusted-roots store the build gate checks.
# Qt-free; depends on io (and core through it). pugixml (ADR 0033) is linked PRIVATE, so no layer
# above project sees pugixml.hpp; FrostLayers' deferred pass checks the $<LINK_ONLY:...> entry a
# static library propagates.
# TODO(M2): MSBuild evaluation through path (c) (MSBuild.exe -getTargetResult; ProjectHost follows in
# M16, ADR 0029), ProjectModelCache, CMake File API, presets, toolchain detection and restricted mode
# join here.

frost_add_library(project LAYER project
    SOURCES
        src/paths.cpp
        src/sln_reader.cpp
        src/slnx_reader.cpp
        src/solution_model.cpp
        src/text_util.cpp
        src/trust_store.cpp
        src/vcxproj_reader.cpp
        src/xml.cpp
    DEPS frost::io)
target_link_libraries(frost_project PRIVATE pugixml::pugixml)
