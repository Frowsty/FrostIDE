# SPDX-License-Identifier: Apache-2.0
#
# Part of the layering fixture: imported targets created in a subdirectory are
# directory-scoped, so the deferred top-level graph check cannot resolve them.
# Qt must still be recognised by name, and any other unresolvable namespaced
# target must be reported rather than silently skipped.

# 6. Qt imported in a subdirectory (find_package without GLOBAL) and linked raw.
add_library(Qt6::Gui INTERFACE IMPORTED)
frost_add_library(bad_subdir_qt LAYER io INTERFACE)
target_link_libraries(frost_bad_subdir_qt INTERFACE Qt6::Gui)

# 7. A third-party package imported in a subdirectory: its graph is invisible.
add_library(Pkg::thing INTERFACE IMPORTED)
frost_add_library(bad_subdir_hidden LAYER io INTERFACE THIRD_PARTY Pkg::thing)
