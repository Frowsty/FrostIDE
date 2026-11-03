// SPDX-License-Identifier: Apache-2.0
// In-process Mono DAP bridge for one already-admitted module and one selected endpoint.
#pragma once
#include "frost/debug/dap_client.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/debug/mono_transport.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace frost::debug::mono {
struct SourcePoint {
    std::uint32_t methodToken = 0;
    std::uint32_t sourceIndex = 0;
    std::int32_t line = 0;
    std::int32_t ilOffset = 0;
};
struct VerifiedModule {
    std::string assemblyPath;
    std::string moduleId;
    std::string image;
    std::string pdb;
    std::vector<std::string> sources;
    std::vector<SourcePoint> points;
};
// The connector must authenticate the selected process and current trusted generation before any
// bytes and for every I/O operation. The owner must revoke the session on editor/source changes.
struct AdapterTarget {
    std::shared_ptr<const VerifiedModule> module;
    std::function<Result<std::unique_ptr<BinaryLoopback>>(MonoDeadline, std::stop_token)> connect;
};
[[nodiscard]] AdapterFactory monoAdapterFactory(AdapterTarget target);
} // namespace frost::debug::mono
