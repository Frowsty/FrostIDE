// SPDX-License-Identifier: Apache-2.0
// Responsive, cancellable ProjectHost plan-only preparation; never commits project edits.
#pragma once

#include "frost/document/refactoring_transaction.hpp"

#include <functional>
#include <stop_token>

class QWidget;

namespace frost::app {

// Owner thread, modal UI pumps while an owned worker performs the existing helper protocol.
// All request inputs are copied before launch; launcher/services outlive this synchronous call.
// Cancelling requests the helper's stop token and retains the dialog until owned work completes.
[[nodiscard]] Result<document::RefactoringPlan>
prepareSourceRenameInBackground(QWidget* ownerWindow,
                                std::function<Result<document::RefactoringPlan>(std::stop_token)> prepare);

} // namespace frost::app
