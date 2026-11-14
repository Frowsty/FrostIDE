// SPDX-License-Identifier: Apache-2.0
// FER-10: composable FrostEngine scene-test provider surface for the existing Unit Tests window.
#pragma once
#include "frost/engine/engine_test_provider.hpp"

#include <functional>
#include <memory>
#include <thread>

class QWidget;
namespace frost::app {
struct EngineUnitTestsSource {
    engine::EngineTestProviderOptions options;
    // Owner increments on project/profile/trust generation changes. No QObject access in admitted.
    std::uint64_t revision = 0;
    std::function<bool()> admitted;                   // Cheap atomic/trust revision checks only, no filesystem calls.
    std::function<Result<void>()> validatePhysical{}; // Optional, worker-only physical/tool capture check.
};
// Pollable worker result. cancel/destruction requests stop without joining; ready becomes true only
// after physical validation returned. takeResult consumes one actual validation result, no QObject.
class EngineTestsValidation final {
public:
    ~EngineTestsValidation();
    void cancel() noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::optional<Result<void>> takeResult();

private:
    friend class EngineTestsRetirementBatch;
    struct State;
    explicit EngineTestsValidation(std::shared_ptr<State> state);
    std::shared_ptr<State> m_state;
};
// Scoped lifetime barrier. GUI retirement never joins a worker; the application MUST drain after
// widget/event-loop teardown, before borrowed fs/launcher/trust/admission services die. No new
// retirements may be submitted after final drain begins. Thread-safe; drain/destructor may block.
class EngineTestsRetirementBatch final {
public:
    EngineTestsRetirementBatch();
    ~EngineTestsRetirementBatch();
    EngineTestsRetirementBatch(const EngineTestsRetirementBatch&) = delete;
    EngineTestsRetirementBatch& operator=(const EngineTestsRetirementBatch&) = delete;
    void retire(std::jthread worker);
    // Starts worker-only physical validation, then rechecks the cheap logical source admission.
    // Caller limits concurrent requests, polls ready, then rechecks its current GUI source/revision.
    [[nodiscard]] std::shared_ptr<EngineTestsValidation> validate(EngineUnitTestsSource source);
    [[nodiscard]] std::size_t pending() const noexcept;
    void drain();

private:
    void collect(std::jthread worker);
    struct State;
    std::shared_ptr<State> m_state;
};
struct EngineUnitTestsWindowContext {
    io::IFileSystem* fs = nullptr;
    io::IProcessLauncher* launcher = nullptr;
    std::shared_ptr<EngineTestsRetirementBatch> retirements; // Required, retained/drained by the owner.
    // GUI-thread snapshot. Error explains missing engine/project/trust. Captures physical identity;
    // admitted must remain thread-safe and reject changed trust/source for the entire worker call.
    std::function<Result<EngineUnitTestsSource>()> source;
    std::function<void(const std::string&, int)> openLocation;
    // GUI only, exact scene selector; FER-9 owner supplies real test-mode debugger launch.
    // Empty means debugging is unavailable. Never called for a runtime-only Test.Case child.
    std::function<void(const engine::EngineTestProviderOptions&, const engine::EngineSceneTest&)> debugScene;
};
// GUI-thread factory, parent owns result. Context services outlive the widget. Worker operations use
// only the Qt-free provider; destruction revokes delivery and retires owned work without joining.
// Source/admitted snapshots must be GUI-safe: physical filesystem work belongs in validatePhysical.
// No separate product window:
// root inserts this surface alongside the existing CTest provider, preserving its commands/gutters.
// Object name engineUnitTestsProvider. Runtime case children are display/navigation only because
// test.run cannot select them. Unknown durations show an em dash, never a guessed zero.
[[nodiscard]] QWidget* createEngineUnitTestsProviderWidget(QWidget* parent, EngineUnitTestsWindowContext context);
} // namespace frost::app
