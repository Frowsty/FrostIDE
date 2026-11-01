// SPDX-License-Identifier: Apache-2.0
//
// The typed settings registry (PLAN.md section 3.6, ADR 0021): every key FrostIDE reads, with its
// schema, default, allowed scopes, `executes` flag and the title, description and category the M15
// Settings dialog is generated from. builtinRegistry() holds FrostIDE's own keys.
/*
 * Scopes, lowest precedence first (a higher scope's valid value wins):
 *   Default          the registry's default value
 *   Application      %APPDATA%\FrostIDE\settings.json, $XDG_CONFIG_HOME/frostide/settings.json
 *   ProjectShared    <root>/.frost/settings.json (committed)
 *   ProjectPersonal  <root>/.frost/workspace.json (gitignored through .frost/.gitignore)
 *   FileLevel        .editorconfig, per file (editorconfig.hpp; only keys that allow it)
 * A value in a scope the key does not allow is ignored (and reported). A key marked `executes` names
 * an executable, command, plugin or script; its values in either project scope are ignored until
 * the project is trusted (PLAN.md section 3.5), whatever scopes it allows. Tool paths and plugin
 * lists are Application-only.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/settings/value.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::settings {

enum class Scope : std::uint8_t { Default, Application, ProjectShared, ProjectPersonal, FileLevel };

// "default", "application", "project-shared", "project-personal", "file".
[[nodiscard]] std::string_view toString(Scope scope) noexcept;

// True for ProjectShared and ProjectPersonal, the scopes the trust rule covers.
[[nodiscard]] constexpr bool isProjectScope(Scope scope) noexcept {
    return scope == Scope::ProjectShared || scope == Scope::ProjectPersonal;
}

// A set of scopes. O(1) operations.
class ScopeMask {
public:
    constexpr ScopeMask() noexcept = default;
    constexpr ScopeMask(std::initializer_list<Scope> scopes) noexcept {
        for (const Scope scope : scopes) {
            m_bits = static_cast<std::uint8_t>(m_bits | bit(scope));
        }
    }
    [[nodiscard]] constexpr bool has(Scope scope) const noexcept { return (m_bits & bit(scope)) != 0; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_bits == 0; }
    [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return m_bits; }
    friend constexpr bool operator==(ScopeMask, ScopeMask) noexcept = default;

private:
    [[nodiscard]] static constexpr std::uint8_t bit(Scope scope) noexcept {
        return static_cast<std::uint8_t>(1U << static_cast<unsigned>(scope));
    }
    std::uint8_t m_bits = 0;
};

// Common scope sets.
inline constexpr ScopeMask kApplicationOnly{Scope::Application};
inline constexpr ScopeMask kApplicationAndPersonal{Scope::Application, Scope::ProjectPersonal};
inline constexpr ScopeMask kAllFileScopes{Scope::Application, Scope::ProjectShared, Scope::ProjectPersonal};
inline constexpr ScopeMask kAllScopesWithEditorConfig{Scope::Application, Scope::ProjectShared, Scope::ProjectPersonal,
                                                      Scope::FileLevel};

// One registry entry.
struct KeySpec {
    // "namespace.camelCase[.more]": [a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9]*)+, at most 128 bytes.
    std::string key;
    std::string title;       // Short English label ("Tab size").
    std::string description; // One or two English sentences; the dialog's help text.
    std::string category;    // Dialog tree path, '/'-separated ("Editor/Code Style").
    Schema schema;
    Value defaultValue;    // Must pass the schema.
    ScopeMask scopes;      // Never Default; at least one of Application, ProjectShared, ProjectPersonal.
    bool executes = false; // Names something that runs (see the file comment).
    bool advanced = false; // Shown under Advanced Settings only.
};

// True when `key` matches the KeySpec::key pattern. O(n).
[[nodiscard]] bool isValidSettingKey(std::string_view key) noexcept;

/*
 * A set of keys. Built once (single-threaded), then read-only: after the first SettingsStore or
 * snapshot refers to it, add() must not be called (indices would shift), and every const member is
 * safe from any thread. Entries keep registration order; lookups use a sorted index.
 */
class Registry {
public:
    Registry() = default;

    /*
     * Registers `spec`. Errors: InvalidArgument (bad key, empty title or description, an empty
     * scope set or one with Default, a default that fails the schema, an Enum without options, a
     * RecordList without fields); AlreadyExists for a duplicate key. O(n).
     */
    [[nodiscard]] Result<void> add(KeySpec spec);

    // The entry for `key`, or nullptr. O(log n).
    [[nodiscard]] const KeySpec* find(std::string_view key) const noexcept;
    // The registration index of `key`. O(log n).
    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view key) const noexcept;
    // Precondition: index < size().
    [[nodiscard]] const KeySpec& at(std::size_t index) const;
    [[nodiscard]] std::span<const KeySpec> keys() const noexcept { return m_keys; }
    [[nodiscard]] std::size_t size() const noexcept { return m_keys.size(); }

    // True when some key starts with `key`'s first segment ("editor" for "editor.fontSzie"), so an
    // unknown key there is probably a typo worth a warning. O(log n).
    [[nodiscard]] bool knowsNamespace(std::string_view key) const noexcept;

private:
    std::vector<KeySpec> m_keys;
    std::vector<std::pair<std::string, std::size_t>> m_index; // Sorted by key.
};

// FrostIDE's own keys (builtin_registry.cpp). Built on first use; thread-safe; lives forever.
[[nodiscard]] const Registry& builtinRegistry();

} // namespace frost::settings
