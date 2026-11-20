# SPDX-License-Identifier: Apache-2.0
# Optional real public-API sample, built only in a later user-authorized build.
frost_add_executable(frost-extension-text-statistics LAYER rpc
    SOURCES main.cpp
    DEPS frost::rpc)
