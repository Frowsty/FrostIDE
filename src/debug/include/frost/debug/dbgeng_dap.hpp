// SPDX-License-Identifier: Apache-2.0
//
// Explicit Windows DbgEng adapter selection for FER-16 (ADR 0081). LLDB remains the default.
// The caller admits the native program, Natvis files, symbol directories and dump against the
// current project's trust before starting this separate adapter process. No DbgEng DLL is shipped.
#pragma once

#include "frost/debug/lldb_dap.hpp"

#include <string>
#include <vector>

namespace frost::debug {

inline constexpr std::string_view kDbgEngAdapterId = "frost-dbgeng";

struct DbgEngTarget {
    DebugTarget native; // program may be empty only for an explicit dumpFile; no process is then claimed.
    std::vector<std::string> natvisFiles;
    std::vector<std::string> localSymbolPaths;
    // Absolute local minidump path. Nonempty selects a read-only postmortem session, never launch.
    std::string dumpFile;
};

// Pure validation/serialization: all paths absolute, bounded string lists, no embedded NUL or
// command separators in debugger path lists. Errors: InvalidArgument; Unsupported (source map).
// Trust is the caller's.
[[nodiscard]] Result<rpc::Json> dbgEngDapLaunchArguments(const DbgEngTarget& target);
// Adapter executable must be absolute and staged by FrostIDE. Stdio is piped; inherited symbol
// server environment is removed. Launch validation and errors belong to IProcessLauncher.
[[nodiscard]] io::ProcessSpec dbgEngDapProcessSpec(std::string executable);
// Starts the adapter through the supplied launcher and the ordinary DAP process transport.
[[nodiscard]] AdapterFactory dbgEngDapAdapterFactory(io::IProcessLauncher& launcher, std::string executable);

} // namespace frost::debug
