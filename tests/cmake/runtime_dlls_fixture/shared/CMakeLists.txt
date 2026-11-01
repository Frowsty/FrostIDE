# SPDX-License-Identifier: Apache-2.0
#
# Part of the runtime-DLL fixture: three consumers in one output directory, the shape
# of tests/sched (three test executables) and tests/app (Qt and non-Qt tests side by
# side). The directory must get the union of their DLLs from a single copy step.
fixture_consumer(shared_a WITH_QT)
fixture_consumer(shared_b WITH_QT)
fixture_consumer(shared_c)
