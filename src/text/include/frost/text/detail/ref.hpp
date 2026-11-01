// SPDX-License-Identifier: Apache-2.0
//
// Intrusive reference counting for frost::text's immutable shared objects (sum-tree nodes and
// the text storage; ADR 0013, D5 and D7). Not public API: detail headers are included by the
// public ones and may change with any slice.
/*
 * RefCounted carries the count; Ref<T> is the owning handle and the only place that allocates
 * (Ref<T>::make) or releases (T::destroy, else delete) such objects.
 *
 * Memory ordering (the scheme Boost documents for intrusive_ptr and in its Boost.Atomic usage
 * examples, "Reference counting"; Boost Software License 1.0):
 *   - Increments are relaxed. A new reference is always made from an existing one, which keeps
 *     the object alive, and the handle it is copied from was itself published to this thread by
 *     some happens-before edge, so no ordering is needed to read the object.
 *   - Decrements are fetch_sub(acq_rel). The release half orders every access a thread made
 *     through its reference before its decrement; the acquire half makes the thread that drops
 *     the count to zero see all of them before it destroys the object. Boost uses a release
 *     decrement plus an acquire fence on the last one; the acq_rel read-modify-write is used
 *     instead because ThreadSanitizer does not model standalone fences.
 * Objects are immutable after construction apart from the count, so nothing else needs ordering.
 *
 * Thread safety: copying, moving and destroying distinct Ref objects that share an object is safe
 * from any threads; one Ref object is not safe to assign while another thread reads it (like
 * std::shared_ptr). Allocation failure terminates the process (the callers are noexcept).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace frost::text::detail {

// Base of every reference-counted object. A new object starts with one reference, which the Ref
// returned by Ref<T>::make adopts. Never copied or moved: the count belongs to one address.
struct RefCounted {
    RefCounted() noexcept = default;
    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;

    // The number of Ref handles to this object. Mutable so that const objects can be shared;
    // atomic because handles on different threads change it concurrently (orders above).
    mutable std::atomic<std::uint32_t> m_refs{1};
};

// An owning handle to a RefCounted T, or null. Copy is O(1) (one relaxed increment); move touches
// no counter; destruction or reassignment drops one reference and, for the last one, destroys the
// object through T::destroy(T*) when T declares it (sum-tree nodes pick their concrete type
// there) or plain delete otherwise. T must be complete wherever a Ref<T> is copied or destroyed.
template<class T>
class Ref {
public:
    constexpr Ref() noexcept = default;

    // Creates a U (T itself or a class derived from T) with `args` and returns the only reference.
    // Allocation failure terminates (noexcept).
    template<class U = T, class... Args>
    [[nodiscard]] static Ref make(Args&&... args) noexcept {
        static_assert(std::is_base_of_v<T, U>, "Ref<T>::make<U>: U must be T or derive from T");
        // Without a virtual destructor, only T::destroy can delete a U through a T*.
        static_assert(
            std::is_same_v<T, U> || requires(T* p) { T::destroy(p); },
            "Ref<T>::make<U> with U != T needs T::destroy(T*)");
        return Ref(static_cast<T*>(new U(std::forward<Args>(args)...)));
    }

    Ref(const Ref& other) noexcept : m_ptr(other.m_ptr) { retain(); }
    Ref(Ref&& other) noexcept : m_ptr(std::exchange(other.m_ptr, nullptr)) {}
    Ref& operator=(const Ref& other) noexcept {
        if (this != &other) {
            Ref copy(other);
            swap(copy);
        }
        return *this;
    }
    Ref& operator=(Ref&& other) noexcept {
        if (this != &other) {
            Ref moved(std::move(other));
            swap(moved);
        }
        return *this;
    }
    ~Ref() { release(); }

    void swap(Ref& other) noexcept { std::swap(m_ptr, other.m_ptr); }
    // Drops the reference (if any) and becomes null.
    void reset() noexcept {
        release();
        m_ptr = nullptr;
    }

    [[nodiscard]] T* get() const noexcept { return m_ptr; }
    T* operator->() const noexcept { return m_ptr; }
    T& operator*() const noexcept { return *m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

    // Identity comparison: the same object (or both null).
    friend bool operator==(const Ref& a, const Ref& b) noexcept { return a.m_ptr == b.m_ptr; }

private:
    explicit Ref(T* adopted) noexcept : m_ptr(adopted) {}

    void retain() const noexcept {
        if (m_ptr != nullptr) {
            m_ptr->m_refs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void release() noexcept {
        if (m_ptr != nullptr && m_ptr->m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if constexpr (requires(T* p) { T::destroy(p); }) {
                T::destroy(m_ptr);
            } else {
                delete m_ptr;
            }
        }
    }

    T* m_ptr = nullptr;
};

} // namespace frost::text::detail
