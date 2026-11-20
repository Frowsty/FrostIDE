// SPDX-License-Identifier: Apache-2.0
// Read-only C2 adapters over existing IDE protocol data and source-owned inspection metadata.
#pragma once
#include "frost/rpc/json.hpp"
#include <functional>
#include <string>
#include <string_view>
namespace frost::agent {
// Pure exact-id lookup in the same source registry used by frost-analyzer and settings.
// Metadata is not an execution result, installed-tool assertion or external-check inventory.
// Thread-safe, O(rule count), owning JSON result; InvalidArgument for bad ids, NotFound when absent.
[[nodiscard]] Result<rpc::Json> inspectionInfo(std::string_view id);
}
