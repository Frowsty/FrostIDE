# SPDX-License-Identifier: Apache-2.0
#
# FrostFuzzResetWork.cmake: a script (cmake -P), not a module. CTest's fuzz.<name>.reset runs it as
# the setup fixture of fuzz.<name> (cmake/FrostFuzz.cmake), so every fuzz run starts from an empty
# work corpus plus the committed one and its coverage and exec/s do not depend on earlier runs.
#
#   cmake -DFROST_FUZZ_WORK=<build>/fuzz-work/<name> -P cmake/FrostFuzzResetWork.cmake
#
# Deletes the directory with everything libFuzzer wrote into it and recreates it empty (libFuzzer
# exits when a corpus directory is missing). Crash artifacts live in fuzz-artifacts/ and are kept.
if(NOT FROST_FUZZ_WORK OR NOT FROST_FUZZ_WORK MATCHES "/fuzz-work/[^/]+$")
    message(FATAL_ERROR "FrostFuzzResetWork.cmake: FROST_FUZZ_WORK '${FROST_FUZZ_WORK}' is not a fuzz-work/<name> directory")
endif()
file(REMOVE_RECURSE "${FROST_FUZZ_WORK}")
file(MAKE_DIRECTORY "${FROST_FUZZ_WORK}")
