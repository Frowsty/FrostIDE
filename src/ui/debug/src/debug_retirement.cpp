// SPDX-License-Identifier: Apache-2.0
// Background DAP detach with an explicit scoped resource drain, independent of Qt pool lifetime.
#include "frost/ui/debug_retirement.hpp"

#include "frost/core/log.hpp"

#include <QThreadPool>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>
namespace frost::ui {
namespace {
std::shared_ptr<std::atomic<std::size_t>> aggregate() {
    static auto value = std::make_shared<std::atomic<std::size_t>>(0);
    return value;
}
} // namespace
struct DebugRetirementBatch::State {
    std::atomic<std::size_t> pending{0};
    std::mutex mutex;
    std::condition_variable idle;
};
DebugRetirementBatch::DebugRetirementBatch() : m_state(std::make_shared<State>()) {}
DebugRetirementBatch::~DebugRetirementBatch() { drain(); }
void DebugRetirementBatch::retire(std::unique_ptr<debug::DebugSession> session, std::chrono::milliseconds grace) {
    if (!session)
        return;
    const auto state = m_state;
    const auto total = aggregate();
    state->pending.fetch_add(1);
    total->fetch_add(1);
    // A job does not own the batch object: final drain remains an explicit caller obligation,
    // before the services borrowed by admission callbacks can be destroyed.
    QThreadPool::globalInstance()->start([session = std::move(session), state, total, grace]() mutable {
        auto detached = session->detachForShutdown(grace);
        if (!detached)
            log::warn("debug", "Attached debugger detach remains unconfirmed: {}", detached.error().message());
        session.reset(); // Closes/joins transport threads before the resource/lifetime barrier opens.
        {
            const std::lock_guard lock(state->mutex);
            state->pending.fetch_sub(1);
            total->fetch_sub(1);
        }
        state->idle.notify_all();
    });
}
std::size_t DebugRetirementBatch::pending() const noexcept { return m_state->pending.load(); }
void DebugRetirementBatch::drain() {
    std::unique_lock lock(m_state->mutex);
    m_state->idle.wait(lock, [this] { return m_state->pending.load() == 0; });
}
std::size_t pendingDebugDetachments() noexcept { return aggregate()->load(); }
} // namespace frost::ui
