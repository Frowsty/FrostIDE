// SPDX-License-Identifier: Apache-2.0
// Private common machinery for read-only binding previews; not an application mutation API.
#pragma once
#include "frost/engine/binding_queries.hpp"
namespace frost::engine::binding_detail {
[[nodiscard]] Result<void> validateRenameRequests(const std::vector<BindingRenameRequest>& requests,
                                                  const BindingQueryAdmission& current, std::stop_token stop);
[[nodiscard]] sched::Task<Result<std::vector<BindingPreviewEdit>>>
collectRenameEdits(std::vector<BindingRenameRequest> requests, std::vector<BindingDocumentCapture> documents,
                   BindingQueryAdmission current, std::stop_token stop);
// Selected-surface planning compares any generated result to actual regeneration before exposing
// a distinct generated-resource intent. The generic preview never permits cache-file edits.
[[nodiscard]] Result<std::vector<BindingPreviewFile>>
combineCapturedPreview(std::span<const BindingPreviewEdit> edits, std::span<const BindingDocumentCapture> documents,
                       bool allowGenerated);
} // namespace frost::engine::binding_detail
