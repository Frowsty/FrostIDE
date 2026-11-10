# SPDX-License-Identifier: Apache-2.0
#
# frost::sched: lanes and aging policy, Chase-Lev work-stealing ThreadPool,
# Task<T> coroutines, timers, cancellation helpers and the deterministic
# SimExecutor (PLAN.md section 2, "Scheduler"; ADR 0002). Qt-free.
# Threads::Threads comes from find_package(Threads) in the top-level CMakeLists.txt.

frost_add_library(sched LAYER sched
    SOURCES
        src/lane.cpp
        src/latest_wins.cpp
        src/resource_governor.cpp
        src/sim_executor.cpp
        src/task.cpp
        src/thread_pool.cpp
    DEPS frost::core
    THIRD_PARTY Threads::Threads)
