# SPDX-License-Identifier: Apache-2.0
#
# Part of the runtime-DLL fixture: a consumer whose RUNTIME_OUTPUT_DIRECTORY is a
# relative path (resolved against this binary directory) naming the same <build>/bin
# that the fixture's top-level consumer names with an absolute path.
fixture_consumer(bin_a WITH_QT)
set_target_properties(bin_a PROPERTIES RUNTIME_OUTPUT_DIRECTORY ../bin)
