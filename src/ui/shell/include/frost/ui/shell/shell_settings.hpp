// SPDX-License-Identifier: Apache-2.0
//
// The shell settings seam (ADR 0038): the main window's remembered layout (tool window
// visibility and sizes, toolbar and status bar visibility) behind an interface that m1/settings
// implements over the typed settings registry. InMemoryShellSettings holds the defaults until then
// and is the test double. Qt-free.
/*
 * The keys m1/settings registers (none of them executes anything):
 *   ui.toolbar.visible     bool, default true, application scope
 *   ui.statusBar.visible   bool, default true, application scope
 *   ui.toolWindows         array of {id, visible, size}, project-personal scope (workspace state,
 *                          like the IDE's per-project window layout); default empty
 * Sizes are logical pixels at 100% zoom; 0 means "the side's default".
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui {

// One tool window's remembered state.
struct ToolWindowLayout {
    std::string id;
    bool visible = false;
    int size = 0; // Width (left, right) or height (bottom) in logical pixels; 0: the default.
    friend bool operator==(const ToolWindowLayout&, const ToolWindowLayout&) = default;
};

// Everything the shell remembers between sessions.
struct ShellLayout {
    bool toolbarVisible = true;
    bool statusBarVisible = true;
    std::vector<ToolWindowLayout> toolWindows;

    // The entry for `id`, or nullptr. O(n).
    [[nodiscard]] const ToolWindowLayout* find(std::string_view id) const noexcept;
    // The entry for `id`, appended (invisible, default size) when missing. O(n).
    ToolWindowLayout& entry(std::string_view id);
    friend bool operator==(const ShellLayout&, const ShellLayout&) = default;
};

/*
 * Where the layout lives. UI thread only; neither call may block on I/O: load() reads the
 * in-memory registry, store() updates it and schedules the write (m1/settings reports a failed
 * write through its own notifications). Passed by reference and never deleted through this
 * interface.
 */
class IShellSettings {
public:
    [[nodiscard]] virtual Result<ShellLayout> load() const = 0;
    [[nodiscard]] virtual Result<void> store(const ShellLayout& layout) = 0;

protected:
    IShellSettings() = default;
    IShellSettings(const IShellSettings&) = default;
    IShellSettings& operator=(const IShellSettings&) = default;
    ~IShellSettings() = default;
};

/*
 * The in-memory store: the defaults until m1/settings lands, and the test double. failNext(code)
 * makes the next load() or store() fail with `code` without reading or changing anything;
 * failNextStore(code) does so for the next store() only. UI thread only.
 */
class InMemoryShellSettings final : public IShellSettings {
public:
    InMemoryShellSettings() = default;
    explicit InMemoryShellSettings(ShellLayout initial) : m_layout(std::move(initial)) {}

    [[nodiscard]] Result<ShellLayout> load() const override;
    [[nodiscard]] Result<void> store(const ShellLayout& layout) override;

    void failNext(ErrorCode code) { m_fault = code; }
    void failNextStore(ErrorCode code) { m_storeFault = code; }
    [[nodiscard]] const ShellLayout& current() const noexcept { return m_layout; }
    [[nodiscard]] std::uint32_t storeCount() const noexcept { return m_stores; }

private:
    ShellLayout m_layout;
    mutable std::optional<ErrorCode> m_fault;
    std::optional<ErrorCode> m_storeFault;
    std::uint32_t m_stores = 0;
};

} // namespace frost::ui
