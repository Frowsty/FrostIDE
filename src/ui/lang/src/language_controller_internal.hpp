// SPDX-License-Identifier: Apache-2.0
//
// Shared internal helpers and coroutine delivery template for LanguageController's translation units.
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui::lang {

namespace detail {

inline QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

inline std::string fileName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

inline std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

inline bool isCSharpPath(std::string_view path) {
    const std::string lower = lowerAscii(path);
    return lower.ends_with(".cs") || lower.ends_with(".csx");
}

template<class T>
sched::Task<void> deliverTaskResult(sched::Task<Result<T>> task, QPointer<LanguageController> self,
                                    sched::LatestWins* slot, sched::LatestWins::Ticket ticket,
                                    std::function<void(Result<T>)> done) {
    auto result = std::make_shared<Result<T>>(co_await std::move(task));
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        co_return;
    }
    QMetaObject::invokeMethod(
        app,
        [self, slot, ticket, done = std::move(done), result]() {
            if (!self || !slot->isCurrent(ticket)) {
                return;
            }
            done(std::move(*result));
            if (self) {
                emit self->requestFinished();
            }
        },
        Qt::QueuedConnection);
}

inline sched::Task<Result<std::vector<::frost::lang::UsageGroup>>>
runFindUsagesTask(::frost::lang::ILspRequester& server, io::IFileSystem& fs,
                  std::vector<std::pair<std::string, text::Snapshot>> open, std::string path,
                  ::frost::lang::LspPosition position, ::frost::lang::PositionEncoding encoding,
                  std::stop_token token) {
    auto references = co_await ::frost::lang::requestReferences(server, path, position, true, token);
    if (!references) {
        co_return std::unexpected(std::move(references.error()));
    }
    std::map<std::string, text::Snapshot, std::less<>> byKey;
    for (auto& [openPath, snapshot] : open) {
        byKey.insert_or_assign(::frost::lang::documentKey(openPath), std::move(snapshot));
    }
    ::frost::lang::FileLineSource lines(fs, [&byKey](std::string_view file) -> std::optional<text::Snapshot> {
        auto it = byKey.find(::frost::lang::documentKey(file));
        return it != byKey.end() ? std::optional<text::Snapshot>(it->second) : std::nullopt;
    });
    co_return ::frost::lang::groupUsages(*references, lines.source(), encoding);
}

} // namespace detail

template<class T>
void LanguageController::start(sched::LatestWins& slot, std::function<sched::Task<Result<T>>(std::stop_token)> make,
                               std::function<void(Result<T>)> done) {
    const sched::LatestWins::Ticket ticket = slot.next();
    sched::Task<Result<T>> task = make(ticket.token);
    m_services.scheduler
        .spawn(sched::Lane::Interactive,
               detail::deliverTaskResult<T>(std::move(task), QPointer<LanguageController>(this), &slot, ticket,
                                            std::move(done)))
        .detach();
}

} // namespace frost::ui::lang
