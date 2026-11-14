// SPDX-License-Identifier: Apache-2.0
// Reusable editor-quality three-way merge; sources are supplied by the Git/CLI owner.
#pragma once
#include "frost/core/error.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include <functional>
#include <string>
#include <string_view>
namespace frost::ui {
struct MergeEditorInputs {
    std::string base, ours, theirs;
    std::string path; // Language identity and window title; this view never opens/writes it.
};
// Called with user-resolved committed UTF-8. Must reject stale source snapshots and revoked trust
// and save atomically. Failure leaves the editable result/history available for recovery.
using MergeEditorApply = std::function<Result<void>(std::string_view)>;
// Modal owner-thread UI over actual Document/EditorView models. Copies sources; sources and
// services' borrowed dependencies outlive the call. false means Cancel, true means Apply succeeded.
// No I/O occurs here; the callback owns admission, encoding, source-identity validation and writing.
[[nodiscard]] Result<bool> showMergeEditor(MergeEditorInputs inputs, MergeEditorApply apply,
    EditorServices services = {}, QWidget* parent = nullptr);
}
