# SPDX-License-Identifier: Apache-2.0
# Licence-check fixture: tests may use Qt Test.
find_package(Qt6 REQUIRED COMPONENTS Test)
target_link_libraries(app_tests PRIVATE Qt6::Test)
