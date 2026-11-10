// SPDX-License-Identifier: Apache-2.0
//
// Actions on save (REF-12): the edits that "Reformat code on save" and "Optimize includes on save"
// make to a C or C++ document before it is written, both computed by clangd. Formatting is
// textDocument/formatting, which clangd answers by running clang-format with the .clang-format
// found above the file (the same library and style lookup as the pinned clang-format CLI).
// Optimizing includes applies clangd's include-cleaner fix (clangd_actions.hpp). Qt-free.
/*
 * Both functions compute byte edits for `snapshot`, the text clangd holds for the document (the
 * caller keeps clangd in sync, as every editor edit reaches ClangdService::changeDocument first),
 * and never apply anything themselves; an empty list means "nothing to change". The caller applies
 * them as one undo step and then saves.
 *
 * Waiting. Saving is synchronous for its callers (Ctrl+S, Save All, the close prompt), so the
 * workbench waits for these requests with waitForResult: the coroutine runs on `scheduler`, the
 * calling thread blocks on a condition variable for at most `timeout`, and on timeout the request
 * is cancelled ($/cancelRequest through the stop token) and Timeout is returned, after which the
 * file is saved unchanged. The calling thread must not be a ThreadPool worker. The coroutine keeps
 * its shared state alive, so a late answer after the timeout is dropped safely.
 *
 * Errors: the requester's (Busy without a running server), toByteEdits' (an edit outside the
 * snapshot), Timeout (waitForResult), Cancelled.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/diagnostics.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"
#include "frost/text/snapshot.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace frost::lang {

// The whole-document formatting edits clangd computes for `path` (whose text is `snapshot`), as
// byte edits sorted for TextBuffer::applyEdits. clangd takes the style from .clang-format and
// ignores `options` where the style file sets them.
[[nodiscard]] sched::Task<Result<std::vector<ByteEdit>>> formatEditsForSave(ILspRequester& server, std::string path,
                                                                            text::Snapshot snapshot,
                                                                            FormattingOptions options,
                                                                            std::stop_token token = {});

/*
 * The include-cleaner fix for `path`: `diagnostics` are clangd's for `snapshot` (the caller drops
 * stale ones); the include-cleaner ones among them are sent with textDocument/codeAction and the
 * fix pickIncludeCleanerFix chooses is returned as byte edits for this file. Empty when clangd
 * reports no unused or missing include or offers no fix.
 */
[[nodiscard]] sched::Task<Result<std::vector<ByteEdit>>>
includeCleanerEditsForSave(ILspRequester& server, std::string path, text::Snapshot snapshot,
                           std::vector<LiveDiagnostic> diagnostics, std::stop_token token = {});

/*
 * Runs `make(token)` on `scheduler` (Lane::Interactive) and blocks the calling thread until it
 * finishes or `timeout` passes; on timeout it stops the token and returns Timeout. See the header
 * comment for the threads and lifetime. Precondition: not on a ThreadPool worker.
 */
template<class T>
[[nodiscard]] Result<T> waitForResult(sched::IScheduler& scheduler,
                                      std::function<sched::Task<Result<T>>(std::stop_token)> make,
                                      std::chrono::milliseconds timeout) {
    struct State {
        std::mutex mutex;
        std::condition_variable done;
        std::optional<Result<T>> result;
    };
    auto state = std::make_shared<State>();
    std::stop_source stop;
    auto relay = [](sched::Task<Result<T>> task, std::shared_ptr<State> shared) -> sched::Task<void> {
        Result<T> value = co_await std::move(task);
        const std::lock_guard lock(shared->mutex);
        shared->result.emplace(std::move(value));
        shared->done.notify_all();
    };
    scheduler.spawn(sched::Lane::Interactive, relay(make(stop.get_token()), state)).detach();
    std::unique_lock lock(state->mutex);
    if (!state->done.wait_for(lock, timeout, [&] { return state->result.has_value(); })) {
        stop.request_stop();
        return fail(ErrorCode::Timeout,
                    "the language server did not answer within " + std::to_string(timeout.count()) + " ms");
    }
    return std::move(*state->result);
}

} // namespace frost::lang
