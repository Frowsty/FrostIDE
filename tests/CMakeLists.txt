# SPDX-License-Identifier: Apache-2.0
#
# Test suites, one directory per module (PLAN.md section 6.1). Unit tests are
# labelled "unit", process-level tests "integration"; verify scripts select by label.
add_subdirectory(compat)
add_subdirectory(core)
if(FROST_BUILD_UI)
    add_subdirectory(app)
endif()
