# SPDX-License-Identifier: Apache-2.0
#
# Part of the runtime-DLL fixture: four executables in one output directory, the shape
# of tests/sched (three test executables), tests/app (Qt and non-Qt tests side by side)
# and tests/perf/s1 (registered and unregistered executables side by side). The
# directory must get the union of their DLLs from a single copy step, and each of them,
# shared_raw included, must be ordered after it.
fixture_consumer(shared_a WITH_QT)
fixture_consumer(shared_b WITH_QT)
fixture_consumer(shared_c)
# A plain add_executable that never calls frost_copy_runtime_dlls.
add_executable(shared_raw "${PROJECT_SOURCE_DIR}/main.cpp")
if(WIN32)
    target_link_libraries(shared_raw PRIVATE fake::extra)
endif()
