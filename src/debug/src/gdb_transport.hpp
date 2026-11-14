// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/debug/gdb_dap.hpp"
namespace frost::debug {
// Preparation happens on the DAP reader, never the factory caller (GUI thread).
std::unique_ptr<IDapTransport> gdbDapTransport(io::IProcessLauncher& launcher, std::string executable,
                                            DapProcessOutput output);
}
