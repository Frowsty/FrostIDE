# SPDX-License-Identifier: Apache-2.0
#
# Part of the runtime-DLL fixture: four executables in one output directory. shared_a,
# shared_b and shared_c are registered, in the shape of tests/sched (three test
# executables) and tests/app (Qt and non-Qt tests side by side); shared_raw is an
# executable whose registration was forgotten (no such executable is left in the
# tree). The directory must get the union of their DLLs from a single copy step, and
# each of them, shared_raw included, must be ordered after it.
fixture_consumer(shared_a WITH_QT)
fixture_consumer(shared_b WITH_QT)
fixture_consumer(shared_c)
# A plain add_executable that never calls frost_copy_runtime_dlls.
add_executable(shared_raw "${PROJECT_SOURCE_DIR}/main.cpp")
if(WIN32)
    target_link_libraries(shared_raw PRIVATE fake::extra)
endif()
