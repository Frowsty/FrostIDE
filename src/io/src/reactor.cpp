// SPDX-License-Identifier: Apache-2.0
//
// The Reactor (frost/io/reactor.hpp; ADR 0020): the io_context threads, the lifecycle lock and the
// drain shutdown, plus the Windows Winsock guard that keeps asio from ever loading ws2_32.dll.

#include "frost/io/reactor.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/utf.hpp"

#include "reactor_impl.hpp" // asio first: it includes winsock2.h before windows.h.
#include "watcher_testing.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <format>
#include <new>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#endif

namespace frost::io {
namespace {

// The reactor whose run() loop the calling thread executes; trivially destructible, so no exit-time code.
thread_local constinit Reactor::Impl* t_currentReactor = nullptr;

#ifdef _WIN32
/*
 * Winsock guard (ADR 0020; verified in the asio-1-32-0 sources). On Windows asio/io_context.hpp includes
 * asio/detail/winsock_init.hpp, which defines in every including TU
 *     static const winsock_init<>& winsock_init_instance = winsock_init<>(false);
 * whose winsock_init_base::startup calls ::WSAStartup when its use count goes from 0 to 1, and
 * io_context has a winsock_init<> member doing the same. Unchecked, every binary linking the reactor
 * would load ws2_32.dll and call WSAStartup during static initialisation, which asio's own comment calls
 * unsafe in a DLL (frost_app.dll's initialisers run under the loader lock).
 *
 * asio documents the escape: construct a winsock_init<>::manual before any of asio's own globals; its
 * manual_startup raises the count without calling WSAStartup, so asio's automatic startups and
 * cleanups only move the counter. A function pointer in .CRT$XCL (the CRT runs the .CRT$XC* entries of
 * a module in section-name order, so before every .CRT$XCU initialiser; asio's comment suggests
 * #pragma init_seg(lib), which is the same section) constructs it in static storage and never destroys
 * it, so the count never returns to zero and asio never calls WSAStartup or WSACleanup. ws2_32 is also
 * delay-loaded in every binary that links frost_io (src/io/CMakeLists.txt), so it is never loaded at
 * all; FileWatcher.WinsockIsNeverLoaded asserts it. The pointer is constant-initialised, so this file
 * has no global constructor.
 * Workaround on asio::detail (asio 1.32.0); remove it when asio offers a switch that disables the
 * automatic Winsock initialisation.
 */
using WinsockManual = asio::detail::winsock_init<>::manual;
alignas(WinsockManual) constinit unsigned char g_winsockManualStorage[sizeof(WinsockManual)] = {};

void __cdecl startWinsockManually() noexcept {
    ::new (static_cast<void*>(g_winsockManualStorage)) WinsockManual(); // Never destroyed (see above).
}

using CrtInitializer = void(__cdecl*)() noexcept;
#pragma section(".CRT$XCL", long, read)
[[gnu::used]] __declspec(allocate(".CRT$XCL")) constinit const CrtInitializer g_winsockGuard = &startWinsockManually;
#endif

// Names the calling thread "<name>-<index>" for debuggers and profilers (best effort).
void nameThread(const std::string& name, unsigned index) noexcept {
    const std::string full = std::format("{}-{}", name, index);
#ifdef _WIN32
    if (auto wide = toWide(full)) {
        static_cast<void>(SetThreadDescription(GetCurrentThread(), wide->c_str()));
    }
#else
    // pthread names hold 15 bytes plus the terminator.
    const std::string cut = full.substr(0, 15);
    static_cast<void>(pthread_setname_np(pthread_self(), cut.c_str()));
#endif
}

// A reactor thread's body: SIGPIPE blocked on POSIX for the thread's lifetime (a pending, blocked,
// thread-directed SIGPIPE is discarded when the thread exits), then io_context::run() until the drain.
void runReactorThread(Reactor::Impl& impl, unsigned index) noexcept {
#ifndef _WIN32
    sigset_t pipeOnly;
    sigemptyset(&pipeOnly);
    sigaddset(&pipeOnly, SIGPIPE);
    static_cast<void>(pthread_sigmask(SIG_BLOCK, &pipeOnly, nullptr));
#endif
    nameThread(impl.options.name, index);
    t_currentReactor = &impl;
    // A handler that throws is a bug (frost::io's handlers use error_code overloads only): report it.
    // The Violation's expression points into this frame, which stays alive because the report aborts.
    std::array<char, 512> message{};
    try {
        static_cast<void>(impl.context.run());
    } catch (const std::exception& exception) {
        const std::string text = std::format("no exception escapes a reactor handler (caught: {})", exception.what());
        std::memcpy(message.data(), text.data(), std::min(text.size(), message.size() - 1));
        contracts::reportViolationAndAbort(
            {contracts::Kind::Assertion, message.data(), std::source_location::current()});
    } catch (...) {
        contracts::reportViolationAndAbort({contracts::Kind::Assertion,
                                            "no exception escapes a reactor handler (caught a non-std exception)",
                                            std::source_location::current()});
    }
    t_currentReactor = nullptr;
}

} // namespace

Reactor::Impl* detail::currentReactor() noexcept { return t_currentReactor; }

Reactor::Impl::Impl(ReactorOptions reactorOptions)
    : options(std::move(reactorOptions)), context(static_cast<int>(options.threads)) {}

Result<void> Reactor::Impl::bind(const std::shared_ptr<detail::BoundState>& state) {
    LockGuard lock(lifecycle);
    if (shutDown) {
        return fail(ErrorCode::Cancelled, "the reactor is shut down");
    }
    // Expired entries are pruned whenever the registry doubles, so binding stays amortised O(1).
    if (bound.size() >= pruneAt) {
        std::erase_if(bound, [](const std::weak_ptr<detail::BoundState>& entry) { return entry.expired(); });
        pruneAt = std::max<std::size_t>(16, bound.size() * 2);
    }
    bound.push_back(state);
    return {};
}

Reactor::Reactor(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

Result<std::unique_ptr<Reactor>> Reactor::create(ReactorOptions options) {
    if (options.threads < 1 || options.threads > 2) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("Reactor: threads must be 1 or 2 (PLAN.md 3.3), got {}", options.threads));
    }
    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>(std::move(options));
        impl->work.emplace(asio::make_work_guard(impl->context));
    } catch (const std::system_error& error) {
        return fail(ErrorCode::IoError, std::format("Reactor: cannot create the io_context: {}", error.what()));
    }
    for (unsigned i = 0; i < impl->options.threads; ++i) {
        try {
            impl->threads.emplace_back(runReactorThread, std::ref(*impl), i);
        } catch (const std::system_error& error) {
            impl->context.stop();
            for (std::thread& thread : impl->threads) {
                thread.join();
            }
            impl->threads.clear();
            impl->joined = true;
            impl->work.reset();
            return fail(ErrorCode::IoError, std::format("Reactor: cannot start thread {}: {}", i, error.what()));
        }
    }
    return std::unique_ptr<Reactor>(new Reactor(std::move(impl)));
}

Reactor::~Reactor() {
    shutdown();
    // Acquire: pairs with the release decrement in ~BoundState (reactor_impl.hpp).
    FROST_ASSERT(m_impl->boundObjects.load(std::memory_order_acquire) == 0);
}

void Reactor::shutdown() {
    FROST_PRE(!onReactorThread());
    Impl& impl = *m_impl;
    if (impl.joined) {
        return;
    }
    // 1. Set the flag; from now on no post from outside a strand is made and no state binds.
    std::vector<std::shared_ptr<detail::BoundState>> live;
    {
        LockGuard lock(impl.lifecycle);
        impl.shutDown = true;
        impl.shutDownFlag.store(true, std::memory_order_relaxed);
        for (const std::weak_ptr<detail::BoundState>& entry : impl.bound) {
            if (std::shared_ptr<detail::BoundState> state = entry.lock()) {
                live.push_back(std::move(state));
            }
        }
        impl.bound.clear();
    }
    // 2. Each live state shuts down on its own strand (cancels, closes, closes its queues).
    for (std::shared_ptr<detail::BoundState>& state : live) {
        asio::post(state->strand(), [state, &impl] {
            impl.countHandler();
            state->shutdownOnStrand();
        });
    }
    live.clear();
    // 3. Without the guard, run() returns once every posted handler and every outstanding operation
    // (cancellations included) has completed.
    impl.work.reset();
    // 4. Join.
    for (std::thread& thread : impl.threads) {
        thread.join();
    }
    impl.threads.clear();
    impl.joined = true;
}

bool Reactor::onReactorThread() const noexcept { return t_currentReactor == m_impl.get(); }

ReactorStats Reactor::stats() const noexcept {
    ReactorStats stats;
    // Acquire on the count for the same reason as ~Reactor; the others are relaxed statistics.
    stats.boundObjects = m_impl->boundObjects.load(std::memory_order_acquire);
    stats.handlersRun = m_impl->handlersRun.load(std::memory_order_relaxed);
    stats.shutDown = m_impl->shutDownFlag.load(std::memory_order_relaxed);
    return stats;
}

/*
 * ReactorStall (watcher_testing.hpp): one handler per reactor thread blocks on a shared gate, so every
 * thread is busy until the stall is released. The constructor waits until all of them are blocked.
 */
struct testing::ReactorStall::Gate {
    Mutex mutex;
    std::condition_variable changed;
    unsigned blocked FROST_GUARDED_BY(mutex) = 0;
    bool released FROST_GUARDED_BY(mutex) = false;
};

testing::ReactorStall::ReactorStall(Reactor& reactor) : m_gate(std::make_shared<Gate>()) {
    Reactor::Impl& impl = reactor.impl();
    const unsigned threads = static_cast<unsigned>(impl.threads.size());
    for (unsigned i = 0; i < threads; ++i) {
        asio::post(impl.context, [gate = m_gate] {
            UniqueLock lock(gate->mutex);
            ++gate->blocked;
            gate->changed.notify_all();
            gate->changed.wait(lock.native(), [&]() FROST_NO_THREAD_SAFETY_ANALYSIS { return gate->released; });
        });
    }
    UniqueLock lock(m_gate->mutex);
    m_gate->changed.wait(lock.native(), [&]() FROST_NO_THREAD_SAFETY_ANALYSIS { return m_gate->blocked == threads; });
}

void testing::ReactorStall::release() noexcept {
    if (!m_gate) {
        return;
    }
    {
        LockGuard lock(m_gate->mutex);
        m_gate->released = true;
    }
    m_gate->changed.notify_all();
    m_gate.reset();
}

testing::ReactorStall::~ReactorStall() { release(); }

namespace {

// testing::BoundProbe's implementation: a BoundState without OS resources.
class BoundProbeState final : public detail::BoundState, public testing::BoundProbe {
public:
    explicit BoundProbeState(Reactor::Impl& reactor) : BoundState(reactor) {}
    void shutdownOnStrand() noexcept override { m_shutdownRan.store(true, std::memory_order_release); }
    // Acquire: pairs with the release store above, so the caller sees the strand's work.
    bool shutdownRan() const noexcept override { return m_shutdownRan.load(std::memory_order_acquire); }
    bool post(std::function<void()> fn) override { return BoundState::post(std::move(fn)); }

private:
    std::atomic<bool> m_shutdownRan{false};
};

} // namespace

Result<std::shared_ptr<testing::BoundProbe>> testing::BoundProbe::bind(Reactor& reactor) {
    auto state = detail::makeBound<BoundProbeState>(reactor.impl());
    if (!state) {
        return std::unexpected(std::move(state.error()));
    }
    return std::shared_ptr<testing::BoundProbe>(std::move(*state));
}

void testing::runOnReactorThread(Reactor& reactor, const std::function<void()>& fn) {
    struct Done {
        Mutex mutex;
        std::condition_variable changed;
        bool done FROST_GUARDED_BY(mutex) = false;
    } done;
    asio::post(reactor.impl().context, [&] {
        fn();
        // Notified under the lock: once the waiter sees `done` it destroys this frame's state.
        LockGuard lock(done.mutex);
        done.done = true;
        done.changed.notify_all();
    });
    UniqueLock lock(done.mutex);
    done.changed.wait(lock.native(), [&]() FROST_NO_THREAD_SAFETY_ANALYSIS { return done.done; });
}

} // namespace frost::io
