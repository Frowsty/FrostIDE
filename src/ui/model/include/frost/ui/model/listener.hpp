// SPDX-License-Identifier: Apache-2.0
//
// ListenerToken and ListenerList: subscription bookkeeping for the appearance seams
// (IAppearanceSettings, IOsAppearanceSource), with RAII unsubscription that stays safe when
// the source is gone first (ADR 0015 decision 12). Qt-free.
//
/*
 * A source keeps its listeners in a table owned through std::shared_ptr; each token holds a
 * std::weak_ptr to that table and its entry id. Destroying or reset()ing a token removes the
 * entry; once the source (and so the table) is gone, that is a no-op.
 *
 * notify() calls the listeners registered when it starts, in registration order, over a
 * snapshot: a listener removed during the notification is not called afterwards (its entry is
 * checked before each call), and one added during it first runs on the next notify(). The
 * snapshot keeps each callable alive while it runs, so a listener may reset its own token.
 *
 * Thread safety: none. Tokens and lists belong to one thread (the GUI thread for the
 * appearance seams); m1/settings' adapter marshals cross-thread changes onto it.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace frost::ui {

namespace detail {
// The type-erased part of a listener table that tokens need: removing an entry.
class ListenerRegistry {
public:
    virtual void remove(std::uint64_t id) noexcept = 0;

protected:
    ListenerRegistry() = default;
    ListenerRegistry(const ListenerRegistry&) = default;
    ListenerRegistry& operator=(const ListenerRegistry&) = default;
    ~ListenerRegistry() = default;
};
} // namespace detail

// A subscription. Move-only; destroying it (or reset()) unsubscribes. A default-constructed
// token subscribes nothing.
class [[nodiscard]] ListenerToken {
public:
    ListenerToken() noexcept = default;
    ListenerToken(std::weak_ptr<detail::ListenerRegistry> registry, std::uint64_t id) noexcept
        : m_registry(std::move(registry)), m_id(id) {}
    ListenerToken(ListenerToken&& other) noexcept
        : m_registry(std::move(other.m_registry)), m_id(std::exchange(other.m_id, 0)) {}
    ListenerToken& operator=(ListenerToken&& other) noexcept {
        if (this != &other) {
            reset();
            m_registry = std::move(other.m_registry);
            m_id = std::exchange(other.m_id, 0);
        }
        return *this;
    }
    ListenerToken(const ListenerToken&) = delete ("a subscription has exactly one owner");
    ListenerToken& operator=(const ListenerToken&) = delete ("a subscription has exactly one owner");
    ~ListenerToken() { reset(); }

    // Unsubscribes now (no-op when empty or when the source is gone). O(listeners).
    void reset() noexcept {
        if (auto registry = m_registry.lock()) {
            registry->remove(m_id);
        }
        m_registry.reset();
        m_id = 0;
    }

    // Whether the token still refers to a live subscription.
    [[nodiscard]] bool active() const noexcept { return m_id != 0 && !m_registry.expired(); }

private:
    std::weak_ptr<detail::ListenerRegistry> m_registry;
    std::uint64_t m_id = 0;
};

// The listener table of a source whose listeners take `Args...`. Single-threaded (see above).
template<class... Args>
class ListenerList {
public:
    using Listener = std::function<void(Args...)>;

    ListenerList() : m_table(std::make_shared<Table>()) {}
    ListenerList(const ListenerList&) = delete ("tokens refer to this table");
    ListenerList& operator=(const ListenerList&) = delete ("tokens refer to this table");

    // Registers `listener` (not null) and returns its token. O(1) amortised.
    [[nodiscard]] ListenerToken add(Listener listener) {
        const std::uint64_t id = m_table->nextId++;
        m_table->entries.push_back({id, std::make_shared<Listener>(std::move(listener))});
        return ListenerToken(std::weak_ptr<detail::ListenerRegistry>(m_table), id);
    }

    // Calls every listener registered now, in registration order, skipping any removed meanwhile.
    // O(listeners^2) worst case for the removal checks; listener counts are small.
    void notify(const Args&... args) const {
        const std::vector<Entry> snapshot = m_table->entries;
        for (const Entry& entry : snapshot) {
            if (m_table->contains(entry.id)) {
                (*entry.listener)(args...);
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_table->entries.size(); }

private:
    struct Entry {
        std::uint64_t id;
        std::shared_ptr<Listener> listener;
    };
    struct Table final : detail::ListenerRegistry {
        void remove(std::uint64_t id) noexcept override {
            std::erase_if(entries, [id](const Entry& e) { return e.id == id; });
        }
        [[nodiscard]] bool contains(std::uint64_t id) const noexcept {
            return std::any_of(entries.begin(), entries.end(), [id](const Entry& e) { return e.id == id; });
        }
        std::vector<Entry> entries;
        std::uint64_t nextId = 1;
    };

    std::shared_ptr<Table> m_table;
};

} // namespace frost::ui
