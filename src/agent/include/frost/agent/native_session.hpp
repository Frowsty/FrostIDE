// SPDX-License-Identifier: Apache-2.0
// Streaming native CLI process ownership over the existing bounded reactor pipes.
#pragma once
#include "frost/agent/protocol.hpp"
#include "frost/io/async_process.hpp"

#include <functional>
#include <memory>
namespace frost::agent {
class NativeSession final {
public:
    struct Observer {
        // Worker callbacks; the UI owner must marshal through a guarded queued connection.
        std::function<bool(Json)> event; // false rejects UI backpressure overflow and terminates the session.
        std::function<void(std::string)> stderrChunk;
        std::function<void(Error)> failure;
        std::function<void(io::ExitStatus)> exited;
    };
    // Only after explicit user consent and a freshly checked auth route. Not a cold-start API.
    [[nodiscard]] static Result<std::unique_ptr<NativeSession>> start(io::IProcessLauncher& launcher,
                                                                      io::AsyncContext context,
                                                                      const NativeLaunch& launch,
                                                                      const Json& freshAuthStatus, Observer observer);
    ~NativeSession();
    [[nodiscard]] Result<void> send(const Json& event);
    // Cancels pipe waits and terminates the owned process tree; asynchronous reactor cleanup.
    void stop();

private:
    struct State;
    explicit NativeSession(std::shared_ptr<State> state);
    static sched::Task<void> read(std::shared_ptr<State> state, bool stderrStream);
    static sched::Task<void> write(std::shared_ptr<State> state);
    static sched::Task<void> wait(std::shared_ptr<State> state);
    std::shared_ptr<State> m;
};
} // namespace frost::agent
