# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::vcs (M5, ADR 0048):
#   vcs_unit_tests  the porcelain v2 parser, the per-file status map and its paths, the checkout
#                   refusal parser and failure messages, finding git, the runner's command line and
#                   environment, the change markers and the side-by-side diff model; label "unit";
#   vcs_git_tests   real git on temporary repositories (GitE2E.*): discovery, every status state,
#                   file versions, the status map after an edit and a commit, commits of selected
#                   files, amend, failing and cancelled hooks, rollback, branches, fetch/pull/push
#                   against a local bare repository, restricted mode and the fixtures against
#                   `git diff --histogram`; label "integration" (it starts git processes, so
#                   RESOURCE_LOCK frost_desktop) with the tag "git" (ctest -L git).
# git is the one CMake found for tests/diff (FROST_GIT_EXECUTABLE); FROST_GIT overrides it at run
# time. The fixture comparison reuses tests/diff's dialect probe (frost_diff_git_support; ADR 0019,
# D6). The fixtures under fixtures/ are byte-exact inputs (REUSE.toml and .gitattributes cover them).

if(FROST_GIT_EXECUTABLE)
    file(TO_CMAKE_PATH "${FROST_GIT_EXECUTABLE}" frost_vcs_git)
else()
    set(frost_vcs_git "")
endif()

frost_add_test(vcs_unit_tests
    SOURCES
        line_changes_test.cpp
        runner_test.cpp
        status_test.cpp
    DEPS frost::vcs frost::diff frost::io frost::core
    LABELS unit)
target_include_directories(vcs_unit_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

frost_add_test(vcs_git_tests
    SOURCES
        git_e2e_test.cpp
    DEPS frost::vcs frost::diff frost::io frost::core frost_diff_git_support
    LABELS integration git)
target_include_directories(vcs_git_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(vcs_git_tests PRIVATE
    FROST_VCS_GIT="${frost_vcs_git}"
    FROST_VCS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
