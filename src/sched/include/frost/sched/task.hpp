// SPDX-License-Identifier: Apache-2.0
//
// Task<T>: FrostIDE's coroutine type (PLAN.md section 3.3: async calls return
// Task<T> and take a std::stop_token).
//
// - Lazy: calling a Task-returning coroutine allocates its frame and runs
//   nothing. The body starts when the task is awaited, spawned
//   (IScheduler::spawn) or passed to syncWait.
// - Symmetric transfer: awaiting a task jumps straight into it, and its
//   completion jumps straight back to the awaiter, so arbitrarily long chains of
//   synchronously completing awaits use constant stack.
// - A task runs on whatever thread resumes it; `co_await scheduler.schedule(lane)`
//   moves it to a scheduler lane.
// - Results: T may be void, any move-constructible object type (move-only types
//   included) or Result<U>. A task is awaited at most once; the result is moved out.
//
// Exceptions never cross a task boundary (PLAN.md section 6.5). If an exception
// escapes the body of a Task<Result<U>>, the task completes with an Error whose
// code is ErrorCode::Internal and whose message carries what(). For any other T
// an escaping exception is a bug that cannot be reported in-band, so it goes
// through the contract-violation handler (frost/core/assert.hpp) and the
// process aborts.
//
// Thread safety: a Task object is owned by one thread at a time (it may be moved
// between threads). Ownership: the Task owns its coroutine frame and destroys it
// in its destructor; a never-started task is destroyed without running.
#pragma once

#include "frost/core/assert.hpp"
#include "frost/core/error.hpp"

#include <concepts>
#include <coroutine>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace frost::sched {

// True for frost::Result<U> (std::expected<U, frost::Error>).
template<class T>
inline constexpr bool kIsResult = false;
template<class U>
inline constexpr bool kIsResult<Result<U>> = true;

template<class T = void>
class Task;

namespace detail {

// The what() of the exception being handled, or a fixed text for non-std exceptions.
// Precondition: called while an exception is being handled (e.g. from unhandled_exception()).
[[nodiscard]] std::string currentExceptionMessage();

// Reports an exception escaping a non-Result task through the contract-violation
// handler and aborts. Precondition as for currentExceptionMessage().
[[noreturn]] void abortOnEscapedException() noexcept;

// The parts of every Task promise that do not depend on T.
class TaskPromiseBase {
public:
    // Lazy start: the body runs only when the task is first resumed by an awaiter.
    [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

    // At the end of the body, transfer control straight to the awaiting coroutine.
    struct FinalAwaiter {
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        template<class Promise>
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) const noexcept {
            return self.promise().m_continuation;
        }
        void await_resume() const noexcept {}
    };
    [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }

    void setContinuation(std::coroutine_handle<> continuation) noexcept { m_continuation = continuation; }

private:
    // Who to resume when the body finishes; noop_coroutine() until awaited.
    std::coroutine_handle<> m_continuation = std::noop_coroutine();
};

template<class T>
class TaskPromise final : public TaskPromiseBase {
public:
    Task<T> get_return_object() noexcept;

    // `co_return value;` (a braced list deduces U = T).
    template<class U = T>
        requires std::constructible_from<T, U&&>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
        m_value.emplace(std::forward<U>(value));
    }

    void unhandled_exception() noexcept {
        if constexpr (kIsResult<T>) {
            m_value.emplace(
                std::unexpected(Error(ErrorCode::Internal, "exception escaped a task: " + currentExceptionMessage())));
        } else {
            abortOnEscapedException();
        }
    }

    // Moves the result out. Precondition: the body has completed and the result
    // was not taken before (checked: a second take is a contract violation).
    [[nodiscard]] T take() {
        FROST_ASSERT(m_value.has_value());
        T value = std::move(*m_value);
        m_value.reset();
        return value;
    }

private:
    std::optional<T> m_value;
};

template<>
class TaskPromise<void> final : public TaskPromiseBase {
public:
    Task<void> get_return_object() noexcept;
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept { abortOnEscapedException(); }
    void take() const noexcept {}
};

} // namespace detail

template<class T>
class [[nodiscard]] Task {
    static_assert(std::is_void_v<T> || (std::is_object_v<T> && std::is_move_constructible_v<T>),
                  "Task<T> needs T to be void or a move-constructible object type");

public:
    using promise_type = detail::TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    // An empty task (valid() == false), e.g. a moved-from one.
    Task() noexcept = default;

    Task(Task&& other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            reset();
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }
    Task(const Task&) = delete ("a task owns its coroutine frame");
    Task& operator=(const Task&) = delete ("a task owns its coroutine frame");

    // Destroys the frame: a never-started task never runs; a completed one frees its result.
    // Precondition: the task is not running or suspended mid-body.
    ~Task() { reset(); }

    // True if this object owns a coroutine.
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(m_handle); }

    // Awaiting starts the body (symmetric transfer) and yields its result.
    // Precondition: valid() and not awaited before (a completed task is detected).
    struct Awaiter {
        Handle handle;
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) const noexcept {
            handle.promise().setContinuation(awaiting);
            return handle;
        }
        T await_resume() const { return handle.promise().take(); }
    };
    [[nodiscard]] Awaiter operator co_await() & noexcept {
        FROST_PRE(valid() && !m_handle.done());
        return Awaiter{m_handle};
    }
    [[nodiscard]] Awaiter operator co_await() && noexcept {
        FROST_PRE(valid() && !m_handle.done());
        return Awaiter{m_handle};
    }

private:
    friend class detail::TaskPromise<T>;
    explicit Task(Handle handle) noexcept : m_handle(handle) {}

    void reset() noexcept {
        if (m_handle) {
            std::exchange(m_handle, {}).destroy();
        }
    }

    Handle m_handle;
};

template<class T>
Task<T> detail::TaskPromise<T>::get_return_object() noexcept {
    return Task<T>(Task<T>::Handle::from_promise(*this));
}

inline Task<void> detail::TaskPromise<void>::get_return_object() noexcept {
    return Task<void>(Task<void>::Handle::from_promise(*this));
}

} // namespace frost::sched
