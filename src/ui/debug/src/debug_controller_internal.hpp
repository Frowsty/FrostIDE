// SPDX-License-Identifier: Apache-2.0
//
// Internal DebugController state and pure helpers shared across debug_controller*.cpp.

#pragma once

#include "frost/core/mutex.hpp"
#include "frost/debug/breakpoints.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include <QMetaObject>
#include <QPointer>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui {

// Shared with the sessions' and the run service's callbacks: posts to the controller while it lives.
struct DebugController::Relay {
    Mutex mutex;
    DebugController* target FROST_GUARDED_BY(mutex) = nullptr;

    // Calls function(controller) on the controller's thread, queued, when session `generation` is
    // still the controller's; ~QObject drops what is still queued.
    template<typename F>
    void post(std::uint64_t generation, F&& function) {
        LockGuard lock(mutex);
        if (target != nullptr) {
            DebugController* self = target;
            QMetaObject::invokeMethod(
                self,
                [self, generation, call = std::forward<F>(function)]() mutable {
                    if (self->m_generation == generation) {
                        call(self);
                    }
                },
                Qt::QueuedConnection);
        }
    }
};

// One attached editor.
struct DebugController::Attached {
    QPointer<EditorView> view;
    std::string path; // Normalised.
    std::unique_ptr<debug::DocumentBreakpointTracker> tracker;
    std::vector<QMetaObject::Connection> connections;
};

[[nodiscard]] inline bool isDebugIdentByte(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || c == '_';
}

[[nodiscard]] inline bool lineContainsVariable(std::string_view lineText, std::string_view varName) noexcept {
    if (varName.empty() || lineText.size() < varName.size()) {
        return false;
    }
    std::size_t pos = 0;
    while ((pos = lineText.find(varName, pos)) != std::string_view::npos) {
        const bool beforeOk = (pos == 0) || !isDebugIdentByte(lineText[pos - 1]);
        const bool afterOk =
            (pos + varName.size() >= lineText.size()) || !isDebugIdentByte(lineText[pos + varName.size()]);
        if (beforeOk && afterOk) {
            return true;
        }
        pos += varName.size();
    }
    return false;
}

} // namespace frost::ui
