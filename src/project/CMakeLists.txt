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
# The MSBuild evaluation of SLN-2/SLN-3 (project_host.hpp): the ProjectHost client, its MSBuild.exe
# fallback and the solution-wide evaluation the Solution Explorer shows; it launches processes
# only through io's IProcessLauncher. ProjectModelCache (SLN-4) keeps those evaluations on disk,
# cl_command_lines (SLN-5) turns MSBuild's GetClCommandLines into clangd's compile commands, and the
# safe solution and project edits of SLN-6 (project_edit.hpp) speak the helper's edit protocol.

frost_add_library(project LAYER project
    SOURCES
        src/cl_command_lines.cpp
        src/cmake_compile_database.cpp
        src/cmake_file_api.cpp
        src/cmake_membership.cpp
        src/generated_sources.cpp
        src/cmake_presets.cpp
        src/cmake_profile.cpp
        src/cmake_syntax.cpp
        src/cmake_view_model.cpp
        src/directory_roles.cpp
        src/json.cpp
        src/paths.cpp
        src/process_environment.cpp
        src/project_edit.cpp
        src/project_edit_plan.cpp
        src/project_host.cpp
        src/project_model_cache.cpp
        src/sln_reader.cpp
        src/standalone_project.cpp
        src/solution_configuration_edit.cpp
        src/slnx_reader.cpp
        src/solution_evaluation.cpp
        src/solution_model.cpp
        src/text_util.cpp
        src/trust_store.cpp
        src/vcxproj_reader.cpp
        src/vcxproj_rename.cpp
        src/xml.cpp
    DEPS frost::io)
target_link_libraries(frost_project PRIVATE pugixml::pugixml yyjson::yyjson)
