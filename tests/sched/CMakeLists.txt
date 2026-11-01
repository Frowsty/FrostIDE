# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::sched, split by label because labels apply per executable:
#   sched_tests         unit:   deque, policy, Task, ThreadPool, SimExecutor, LatestWins
#   sched_stress_tests  stress: 10k SimExecutor seeds and the 1M-task real-pool run
#                       (default scale in -Full gates; FROST_STRESS_FULL=1 selects the M0 exit scale)
#   sched_perf_tests    perf:   interactive dispatch p99 (asserted only in optimised, unsanitised builds)
# Parallel ctest (frost_add_test's policy; ADR 0027 decision 3): sched_tests starts thread pools
# (PROCESSORS 2), the stress runs saturate every lane (PROCESSORS 4; they run in -Full only), and
# the perf test asserts dispatch latency in Release, so it runs with nothing beside it (RUN_SERIAL,
# the perf label's default).
frost_add_test(sched_tests
    SOURCES
        deque_test.cpp
        lane_policy_test.cpp
        latest_wins_test.cpp
        sim_executor_test.cpp
        task_test.cpp
        thread_pool_test.cpp
    DEPS frost::sched
    LABELS unit
    PROCESSORS 2)
target_include_directories(sched_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

frost_add_test(sched_stress_tests
    SOURCES
        stress_test.cpp
    DEPS frost::sched
    LABELS stress
    PROCESSORS 4)
target_include_directories(sched_stress_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

frost_add_test(sched_perf_tests
    SOURCES
        dispatch_perf_test.cpp
    DEPS frost::sched
    LABELS perf)
target_include_directories(sched_perf_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
