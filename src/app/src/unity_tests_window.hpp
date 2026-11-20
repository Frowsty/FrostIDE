// SPDX-License-Identifier: Apache-2.0
// Declares the Unity test window factory and its editor control callbacks.
#pragma once
#include "unity_control.hpp"

#include <functional>
#include <vector>
class QWidget;
namespace frost::app {
struct UnityTestCase {
    std::string id, name, fullName, outcome, message, stack;
    double duration = 0;
    bool truncated = false;
};
struct UnityTestObservation {
    std::string owner, job, catalog, mode, state, snapshot, message, execution;
    int total = 0, offset = 0, next = 0;
    std::vector<UnityTestCase> cases;
};
[[nodiscard]] Result<UnityTestObservation> parseUnityTestState(const lang::Json& response);
[[nodiscard]] Result<UnityTestObservation> parseUnityTestPage(const lang::Json& response, std::string_view snapshot,
                                                              int offset);
struct UnityTestsWindowContext {
    std::function<std::optional<UnityEditorIdentity>()> selected{};
    std::function<void(const UnityEditorIdentity&, const lang::Json&, std::function<void(Result<lang::Json>)>)>
        request{};
    std::function<Result<void>()> prepare{};
    std::function<void(const std::string&, int)> openLocation{};
};
[[nodiscard]] QWidget* createUnityTestsWidget(QWidget* parent, UnityTestsWindowContext context);
} // namespace frost::app
