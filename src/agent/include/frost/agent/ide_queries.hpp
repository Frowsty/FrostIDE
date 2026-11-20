// SPDX-License-Identifier: Apache-2.0
// Read-only C2 adapters over existing IDE protocol data and source-owned inspection metadata.
#pragma once
#include "frost/rpc/json.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
namespace frost::agent {
// Pure exact-id lookup in the same source registry used by frost-analyzer and settings.
// Metadata is not an execution result, installed-tool assertion or external-check inventory.
// Thread-safe, O(rule count), owning JSON result; InvalidArgument for bad ids, NotFound when absent.
[[nodiscard]] Result<rpc::Json> inspectionInfo(std::string_view id);
using IdeQueryDone = std::function<void(Result<rpc::Json>)>;
// The adapter calls/replies on one owner thread; it may reply inline. No process/thread is owned here.
using IdeQueryRequest = std::function<void(std::string method, rpc::Json params, IdeQueryDone done)>;
using IdeResourceAdmission = std::function<bool(std::string_view uri)>;
// Removes out-of-scope locations and opaque provider data from an IDE result.
[[nodiscard]] std::optional<rpc::Json> scopedLocations(const rpc::Json& value, const IdeResourceAdmission& admitted);
// Queries actual prepare + both one-hop edge directions. Preserves server data privately in follow-up
// requests, strips opaque data from output, and filters every root/edge resource through admission.
// Emits flat root/relation records so ordinary MCP pagination can split large edge sets.
// At most eight roots, 128 edges/direction and 1 MiB retained result; one owning completion. Errors
// propagate from the real provider or report malformed/excessive data. O(returned JSON bytes).
void hierarchyEdges(bool typeHierarchy, rpc::Json prepareParams, IdeQueryRequest request, IdeResourceAdmission admitted,
                    IdeQueryDone done);
} // namespace frost::agent
