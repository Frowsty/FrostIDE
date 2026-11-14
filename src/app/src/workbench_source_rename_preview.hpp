// SPDX-License-Identifier: Apache-2.0
// Responsive, cancellable ProjectHost plan-only preparation; never commits project edits.
#pragma once

#include "frost/document/refactoring_transaction.hpp"
#include "frost/project/solution_model.hpp"

#include <functional>
#include <stop_token>

class QWidget;

namespace frost::app {

// Owner thread, modal UI pumps while an owned worker performs the existing helper protocol.
// All request inputs are copied before launch; launcher/services outlive this synchronous call.
// Cancelling requests the helper's stop token and retains the dialog until owned work completes.
[[nodiscard]] Result<document::RefactoringPlan>
prepareSourceRenameInBackground(QWidget* ownerWindow,
                                std::function<Result<document::RefactoringPlan>(std::stop_token)> prepare,
                                std::string title = {});

// Synchronous owner-thread API; only filesystem/static parsing runs on the owned worker.
// Cancellation keeps the modal dialog alive until cooperative work ends; no GUI-thread wait
// on an incomplete parse and no partial model publication. One individual IO/XML parse is not
// preemptible. The filesystem must outlive this call (as required by WorkbenchServices).
[[nodiscard]] Result<project::SolutionModel> loadSolutionInBackground(QWidget* ownerWindow, io::IFileSystem& fs,
                                                                      std::string path,
                                                                      project::SolutionLoadOptions options,
                                                                      std::stop_source cancellation);

} // namespace frost::app
