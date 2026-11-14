// SPDX-License-Identifier: Apache-2.0
// FER-10 worker retirement: responsive widget destruction plus explicit borrowed-service drain.
#include "engine_unit_tests_window.hpp"
#include <QThreadPool>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>
namespace frost::app {
struct EngineTestsRetirementBatch::State {
    std::atomic<std::size_t> pending{0};
    std::mutex mutex;
    std::condition_variable idle;
};
EngineTestsRetirementBatch::EngineTestsRetirementBatch() : m_state(std::make_shared<State>()) {}
EngineTestsRetirementBatch::~EngineTestsRetirementBatch() { drain(); }
void EngineTestsRetirementBatch::retire(std::jthread worker) {
    worker.request_stop();
    collect(std::move(worker));
}
void EngineTestsRetirementBatch::collect(std::jthread worker) {
    if (!worker.joinable())
        return;
    const auto state = m_state;
    state->pending.fetch_add(1);
    QThreadPool::globalInstance()->start([state, worker = std::move(worker)]() mutable {
        worker.join();
        {
            const std::lock_guard lock(state->mutex);
            state->pending.fetch_sub(1);
        }
        state->idle.notify_all();
    });
}
struct EngineTestsValidation::State {
    std::stop_source stop;
    std::atomic<bool> ready{false};
    std::mutex mutex;
    std::optional<Result<void>> result;
};
EngineTestsValidation::EngineTestsValidation(std::shared_ptr<State> state) : m_state(std::move(state)) {}
EngineTestsValidation::~EngineTestsValidation() { cancel(); }
void EngineTestsValidation::cancel() noexcept { m_state->stop.request_stop(); }
bool EngineTestsValidation::ready() const noexcept { return m_state->ready.load(); }
std::optional<Result<void>> EngineTestsValidation::takeResult() {
    const std::lock_guard lock(m_state->mutex);
    auto result = std::exchange(m_state->result, std::nullopt);
    if (result && m_state->stop.stop_requested())
        return Result<void>(fail(ErrorCode::Cancelled, "Engine test validation cancelled"));
    return result;
}
std::shared_ptr<EngineTestsValidation> EngineTestsRetirementBatch::validate(EngineUnitTestsSource source) {
    auto state = std::make_shared<EngineTestsValidation::State>();
    auto handle = std::shared_ptr<EngineTestsValidation>(new EngineTestsValidation(state));
    collect(std::jthread([state, source = std::move(source)] {
        Result<void> result;
        if (state->stop.stop_requested())
            result = fail(ErrorCode::Cancelled, "Engine test validation cancelled");
        else if (!source.admitted || !source.admitted())
            result = fail(ErrorCode::Untrusted, "Engine test source admission changed");
        else if (source.validatePhysical)
            result = source.validatePhysical();
        if (state->stop.stop_requested())
            result = fail(ErrorCode::Cancelled, "Engine test validation cancelled");
        else if (!source.admitted || !source.admitted())
            result = fail(ErrorCode::Untrusted, "Engine test source admission changed");
        {
            const std::lock_guard lock(state->mutex);
            state->result = std::move(result);
            state->ready.store(true);
        }
    }));
    return handle;
}
std::size_t EngineTestsRetirementBatch::pending() const noexcept { return m_state->pending.load(); }
void EngineTestsRetirementBatch::drain() {
    std::unique_lock lock(m_state->mutex);
    m_state->idle.wait(lock, [this] { return m_state->pending.load() == 0; });
}
} // namespace frost::app
