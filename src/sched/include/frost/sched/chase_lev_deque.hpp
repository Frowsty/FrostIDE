// SPDX-License-Identifier: Apache-2.0
//
// ChaseLevDeque<T>: the per-worker work-stealing deque of frost::sched.
//
// One owner thread pushes and pops at the bottom (LIFO, cache-warm); any number
// of thief threads steal from the top (FIFO, oldest first). The buffer is a
// growable ring of atomic pointers. Algorithm: D. Chase and Y. Lev, "Dynamic
// Circular Work-Stealing Deque" (SPAA 2005). Memory orderings follow N. M. Lê,
// A. Pop, A. Cohen and F. Zappa Nardelli, "Correct and Efficient Work-Stealing
// for Weak Memory Models" (PPoPP 2013), figure 1 (the C11 version). This is a
// clean-room implementation written from the paper; no other implementation
// was consulted.
//
// TODO(M1): a Relacy-style model check of push/pop/steal (PLAN.md section 6.1).
#pragma once

#include "frost/core/assert.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// ThreadSanitizer does not model std::atomic_thread_fence, so under TSan the
// fence-based publication of slots in the paper looks like a data race on the
// payload. TSan builds therefore also make the slot accesses release/acquire.
// That only strengthens the paper's orderings, so its correctness argument
// still holds; on x86-64 both variants compile to the same plain moves. The
// price: TSan cannot check the release build's fence-based path (ADR 0002).
#if defined(__SANITIZE_THREAD__)
#define FROST_SCHED_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FROST_SCHED_TSAN 1
#endif
#endif
#ifndef FROST_SCHED_TSAN
#define FROST_SCHED_TSAN 0
#endif

namespace frost::sched {

template<class T>
class ChaseLevDeque {
public:
    enum class StealStatus : std::uint8_t {
        Empty,   // Nothing to steal.
        Abort,   // Lost a race with the owner or another thief; retrying may succeed.
        Success, // `item` holds the stolen element.
    };
    struct StealResult {
        StealStatus status;
        T* item; // Non-null exactly when status == Success.
    };

    // Creates an empty deque. `initialCapacity` is rounded up to a power of two (at least 2).
    explicit ChaseLevDeque(std::size_t initialCapacity = 64) {
        std::int64_t capacity = 2;
        while (capacity < static_cast<std::int64_t>(initialCapacity)) {
            capacity *= 2;
        }
        m_current = std::make_unique<Ring>(capacity);
        m_ring.store(m_current.get(), std::memory_order_relaxed);
    }

    // Precondition: no thread is using the deque. Remaining elements are not owned and are not freed.
    ~ChaseLevDeque() = default;

    ChaseLevDeque(const ChaseLevDeque&) = delete ("the owner and thieves share one deque by reference");
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete ("the owner and thieves share one deque by reference");

    // Owner only. Appends `item` (non-null) at the bottom; grows the ring when full.
    // Amortised O(1); growth copies the live elements, O(size).
    void push(T* item) {
        FROST_PRE(item != nullptr);
        // Relaxed: only the owner writes m_bottom, so it reads its own last store.
        const std::int64_t b = m_bottom.load(std::memory_order_relaxed);
        // Acquire (paper): pairs with a thief's successful CAS on m_top, so that
        // thief's read of slot t happens-before we overwrite that slot after a wrap.
        const std::int64_t t = m_top.load(std::memory_order_acquire);
        // Relaxed: only the owner writes m_ring.
        Ring* ring = m_ring.load(std::memory_order_relaxed);
        if (b - t > ring->capacity - 1) {
            ring = grow(ring, b, t);
        }
        // Relaxed in the paper: the release fence below orders it before the bottom store.
        ring->at(b).store(item, kSlotStore);
        // Release fence (paper): every later store to m_bottom, including pop()'s,
        // publishes this slot to a thief that acquire-loads m_bottom.
        std::atomic_thread_fence(std::memory_order_release);
        // Relaxed (paper): the fence above provides the ordering.
        m_bottom.store(b + 1, std::memory_order_relaxed);
    }

    // Owner only. Removes and returns the bottom (newest) element, or nullptr if empty. O(1).
    [[nodiscard]] T* pop() noexcept {
        // Relaxed: owner-private reads, as in push().
        const std::int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
        Ring* ring = m_ring.load(std::memory_order_relaxed);
        // Relaxed (paper): reserve slot b; the seq_cst fence orders it before reading m_top.
        m_bottom.store(b, std::memory_order_relaxed);
        // Seq_cst fence (paper): with the fence in steal() this forms the Dekker
        // pattern that stops the owner and a thief both taking the last element.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // Relaxed (paper): ordered by the fence above.
        std::int64_t t = m_top.load(std::memory_order_relaxed);
        if (t > b) {
            // Empty: restore bottom. Relaxed (paper).
            m_bottom.store(b + 1, std::memory_order_relaxed);
            return nullptr;
        }
        // Relaxed (paper): the owner wrote this slot itself.
        T* item = ring->at(b).load(std::memory_order_relaxed);
        if (t == b) {
            // Last element: race the thieves for it with the same CAS they use.
            // Seq_cst on success (paper), relaxed on failure (the value is discarded).
            if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                item = nullptr;
            }
            // Relaxed (paper): the deque is empty either way; restore the canonical bottom == top.
            m_bottom.store(b + 1, std::memory_order_relaxed);
        }
        return item;
    }

    // Any thread (the owner included). Removes the top (oldest) element. O(1).
    [[nodiscard]] StealResult steal() noexcept {
        // Acquire (paper): pairs with other thieves' CAS on m_top.
        std::int64_t t = m_top.load(std::memory_order_acquire);
        // Seq_cst fence (paper): the other half of pop()'s Dekker pattern.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // Acquire (paper): synchronises with the owner's release fence in push(), making slot t visible.
        const std::int64_t b = m_bottom.load(std::memory_order_acquire);
        if (t >= b) {
            return {StealStatus::Empty, nullptr};
        }
        // Acquire (consume in the paper, which compilers promote to acquire): pairs
        // with the release store in grow(), so a new ring's copied slots are visible.
        Ring* ring = m_ring.load(std::memory_order_acquire);
        // Relaxed (paper); acquire under TSan (see FROST_SCHED_TSAN).
        T* item = ring->at(t).load(kSlotLoad);
        // Seq_cst on success (paper): claims slot t against the owner and other
        // thieves; relaxed on failure because the result is discarded.
        if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return {StealStatus::Abort, nullptr};
        }
        return {StealStatus::Success, item};
    }

    // Any thread. A snapshot of the element count; stale as soon as it returns. O(1).
    [[nodiscard]] std::size_t sizeApprox() const noexcept {
        // Relaxed: an estimate only, never used for synchronisation.
        const std::int64_t b = m_bottom.load(std::memory_order_relaxed);
        const std::int64_t t = m_top.load(std::memory_order_relaxed);
        return b > t ? static_cast<std::size_t>(b - t) : 0;
    }

    // Owner only. The current ring capacity in elements.
    [[nodiscard]] std::size_t capacity() const noexcept {
        return static_cast<std::size_t>(m_ring.load(std::memory_order_relaxed)->capacity);
    }

private:
#if FROST_SCHED_TSAN
    static constexpr std::memory_order kSlotStore = std::memory_order_release;
    static constexpr std::memory_order kSlotLoad = std::memory_order_acquire;
#else
    static constexpr std::memory_order kSlotStore = std::memory_order_relaxed;
    static constexpr std::memory_order kSlotLoad = std::memory_order_relaxed;
#endif

    // A power-of-two ring of slots indexed by the unbounded 64-bit positions top/bottom.
    struct Ring {
        explicit Ring(std::int64_t size)
            : capacity(size), slots(std::make_unique<std::atomic<T*>[]>(static_cast<std::size_t>(size))) {}
        std::atomic<T*>& at(std::int64_t index) noexcept {
            return slots[static_cast<std::size_t>(index & (capacity - 1))];
        }
        const std::int64_t capacity;
        const std::unique_ptr<std::atomic<T*>[]> slots;
    };

    // Owner only: replaces `old` with a ring twice as large holding elements [t, b).
    // Thieves may still be reading `old`, so it is retired, not freed; retired
    // rings are freed with the deque. Their total size is below the live ring's,
    // so memory stays under twice the peak capacity.
    Ring* grow(Ring* old, std::int64_t b, std::int64_t t) {
        auto bigger = std::make_unique<Ring>(old->capacity * 2);
        for (std::int64_t i = t; i < b; ++i) {
            // Relaxed load: owner-written slots. Store as in push(); the release
            // store of m_ring below publishes the copies.
            bigger->at(i).store(old->at(i).load(std::memory_order_relaxed), kSlotStore);
        }
        Ring* raw = bigger.get();
        m_retired.push_back(std::move(m_current));
        m_current = std::move(bigger);
        // Release: publishes the copied slots to thieves that acquire-load m_ring.
        m_ring.store(raw, std::memory_order_release);
        return raw;
    }

    // Top and bottom live on separate cache lines: thieves hammer m_top, the owner m_bottom.
    alignas(64) std::atomic<std::int64_t> m_top{0};
    alignas(64) std::atomic<std::int64_t> m_bottom{0};
    std::atomic<Ring*> m_ring{nullptr};
    std::unique_ptr<Ring> m_current;              // Owner only: the live ring.
    std::vector<std::unique_ptr<Ring>> m_retired; // Owner only: rings thieves may still read.
};

} // namespace frost::sched
