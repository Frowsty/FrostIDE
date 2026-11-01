// SPDX-License-Identifier: Apache-2.0
//
// Keymap format v1: the strict parser and canonical formatter, per-platform resolution
// with the macOS Ctrl/Meta derivation, the built-in keymap entry point, and the
// validation and sanitising of user preferences (keymap.hpp; ADR 0016 decision 8).

#include "frost/input/keymap.hpp"

#include "frost/core/assert.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::input {

KeymapPlatform currentKeymapPlatform() noexcept {
#if defined(_WIN32)
    return KeymapPlatform::Windows;
#elif defined(__APPLE__)
    return KeymapPlatform::MacOS;
#else
    return KeymapPlatform::Linux;
#endif
}

namespace {

constexpr std::size_t kMaxKeymapBytes = std::size_t{1} << 20;
constexpr std::size_t kMaxLineBytes = 4096;
constexpr std::size_t kMaxKeymapIdBytes = 64;
constexpr std::size_t kMaxKeymapNameBytes = 128;

constexpr std::array<std::string_view, 4> kSectionNames = {"default", "windows", "linux", "macos"};

std::unexpected<Error> corrupt(std::size_t line, std::size_t column, std::string_view reason) {
    return fail(ErrorCode::Corrupt, std::format("keymap line {}, column {}: {}", line, column, reason));
}

bool isBlank(char c) noexcept { return c == ' ' || c == '\t'; }

std::string_view trimRight(std::string_view s) noexcept {
    while (!s.empty() && isBlank(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

bool isValidKeymapId(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxKeymapIdBytes || id[0] < 'a' || id[0] > 'z') {
        return false;
    }
    return std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// Swaps Ctrl and Meta in every stroke (the macOS derivation); gestures are unchanged.
Sequence swapCtrlMeta(const Sequence& sequence) noexcept {
    if (sequence.isGesture() || sequence.length() == 0) {
        return sequence;
    }
    const auto swap = [](Stroke s) {
        Modifiers mods = s.mods.without(Modifier::Ctrl).without(Modifier::Meta);
        if (s.mods.has(Modifier::Ctrl)) {
            mods = mods.with(Modifier::Meta);
        }
        if (s.mods.has(Modifier::Meta)) {
            mods = mods.with(Modifier::Ctrl);
        }
        return Stroke{mods, s.key};
    };
    return sequence.length() == 1 ? Sequence::of(swap(sequence.first()))
                                  : Sequence::of(swap(sequence.first()), swap(sequence.second()));
}

// True when `s` is Strict-valid: it formats and parses back to itself.
bool isStrictValid(const Sequence& s) {
    if (!s.valid()) {
        return false;
    }
    const auto parsed = parseSequence(formatSequence(s), SyntaxMode::Strict);
    return parsed && *parsed == s;
}

} // namespace

Result<Keymap> parseKeymap(std::string_view text) {
    if (text.size() > kMaxKeymapBytes) {
        return fail(ErrorCode::OutOfRange, std::format("keymap is {} bytes; the limit is 1 MiB", text.size()));
    }
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }

    Keymap keymap;
    bool haveHeader = false;
    bool haveId = false;
    bool haveName = false;
    int section = -1;
    // Per section, the action ids seen so far (sorted), for duplicate detection.
    std::array<std::vector<std::string_view>, 4> seenActions;

    std::size_t lineNumber = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = text.substr(start, end - start);
        ++lineNumber;
        const bool lastLine = newline == std::string_view::npos;
        start = end + 1;
        if (lastLine && line.empty()) {
            break;
        }
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.size() > kMaxLineBytes) {
            return corrupt(lineNumber, kMaxLineBytes + 1, "line longer than 4096 bytes");
        }
        if (const std::size_t bad = detail::invalidUtf8At(line); bad != std::string_view::npos) {
            return corrupt(lineNumber, bad + 1, "invalid UTF-8");
        }
        for (std::size_t i = 0; i < line.size(); ++i) {
            const auto c = static_cast<unsigned char>(line[i]);
            if (c == 0) {
                return corrupt(lineNumber, i + 1, "NUL byte");
            }
            if ((c < 0x20 && c != '\t') || c == 0x7F) {
                return corrupt(lineNumber, i + 1, "control character");
            }
        }

        // Blank lines and full-line comments.
        std::size_t indent = 0;
        while (indent < line.size() && isBlank(line[indent])) {
            ++indent;
        }
        if (indent == line.size() || line[indent] == '#') {
            continue;
        }
        line = trimRight(line);

        if (!haveHeader) {
            if (line == "frost-keymap 1") {
                haveHeader = true;
                continue;
            }
            const std::string_view version = line.starts_with("frost-keymap ") ? line.substr(13) : std::string_view();
            if (!version.empty() && std::ranges::all_of(version, [](char c) { return c >= '0' && c <= '9'; })) {
                return fail(ErrorCode::Unsupported,
                            std::format("keymap line {}: unsupported format version '{}'", lineNumber, version));
            }
            return corrupt(lineNumber, 1, "expected the header 'frost-keymap 1'");
        }
        if (indent != 0) {
            return corrupt(lineNumber, 1, "unexpected indentation");
        }
        if (line == "frost-keymap" || line.starts_with("frost-keymap ")) {
            return corrupt(lineNumber, 1, "duplicate header");
        }

        // Directives ("id" or "name" alone is a directive with an empty value).
        const bool isIdDirective = line == "id" || line.starts_with("id ") || line.starts_with("id\t");
        const bool isNameDirective = line == "name" || line.starts_with("name ") || line.starts_with("name\t");
        if (isIdDirective || isNameDirective) {
            if (section >= 0) {
                return corrupt(lineNumber, 1, "directive after a section");
            }
            std::size_t valueStart = isIdDirective ? 2 : 4;
            while (valueStart < line.size() && isBlank(line[valueStart])) {
                ++valueStart;
            }
            const std::string_view value = line.substr(valueStart);
            if (isIdDirective) {
                if (haveId) {
                    return corrupt(lineNumber, 1, "duplicate id directive");
                }
                if (!isValidKeymapId(value)) {
                    return corrupt(lineNumber, valueStart + 1, "keymap ids match [a-z][a-z0-9-]* (at most 64 bytes)");
                }
                keymap.m_id = std::string(value);
                haveId = true;
            } else {
                if (haveName) {
                    return corrupt(lineNumber, 1, "duplicate name directive");
                }
                if (value.empty() || value.size() > kMaxKeymapNameBytes) {
                    return corrupt(lineNumber, valueStart + 1, "a keymap name is 1-128 bytes");
                }
                keymap.m_name = std::string(value);
                haveName = true;
            }
            continue;
        }

        // Section headers.
        if (line.front() == '[') {
            if (!haveId || !haveName) {
                return corrupt(lineNumber, 1, haveId ? "missing name directive" : "missing id directive");
            }
            if (line.back() != ']') {
                return corrupt(lineNumber, line.size(), "expected ']'");
            }
            const std::string_view name = line.substr(1, line.size() - 2);
            const auto it = std::ranges::find(kSectionNames, name);
            if (it == kSectionNames.end()) {
                return corrupt(lineNumber, 2, std::format("unknown section '{}'", name));
            }
            const auto index = static_cast<std::size_t>(it - kSectionNames.begin());
            if (keymap.m_present[index]) {
                return corrupt(lineNumber, 1, std::format("duplicate section [{}]", name));
            }
            keymap.m_present[index] = true;
            section = static_cast<int>(index);
            continue;
        }

        // Bindings.
        if (section < 0) {
            return corrupt(lineNumber, 1, "binding outside a section");
        }
        std::size_t idEnd = 0;
        while (idEnd < line.size() && !isBlank(line[idEnd]) && line[idEnd] != '=') {
            ++idEnd;
        }
        const std::string_view actionId = line.substr(0, idEnd);
        if (!isValidActionId(actionId)) {
            return corrupt(lineNumber, 1,
                           std::format("invalid action id '{}' (pattern [a-z][a-zA-Z0-9]*(.[a-z][a-zA-Z0-9]*)+, at "
                                       "most 64 bytes)",
                                       actionId));
        }
        std::size_t pos = idEnd;
        while (pos < line.size() && isBlank(line[pos])) {
            ++pos;
        }
        if (pos == line.size() || line[pos] != '=') {
            return corrupt(lineNumber, pos + 1, "expected '='");
        }
        ++pos;
        auto& seen = seenActions[static_cast<std::size_t>(section)];
        const auto seenAt = std::ranges::lower_bound(seen, actionId);
        if (seenAt != seen.end() && *seenAt == actionId) {
            return corrupt(lineNumber, 1, std::format("action '{}' appears twice in this section", actionId));
        }
        seen.insert(seenAt, actionId);

        Binding binding;
        binding.actionId = std::string(actionId);
        while (pos < line.size() && isBlank(line[pos])) {
            ++pos;
        }
        if (pos < line.size()) {
            while (true) {
                const std::size_t bar = line.find('|', pos);
                const std::size_t pieceEnd = bar == std::string_view::npos ? line.size() : bar;
                std::size_t pieceStart = pos;
                while (pieceStart < pieceEnd && isBlank(line[pieceStart])) {
                    ++pieceStart;
                }
                const std::string_view piece = trimRight(line.substr(pieceStart, pieceEnd - pieceStart));
                if (piece.empty()) {
                    return corrupt(lineNumber, pieceStart + 1, "empty sequence");
                }
                auto sequence = parseSequence(piece, SyntaxMode::Strict);
                if (!sequence) {
                    return corrupt(lineNumber, pieceStart + 1, sequence.error().message());
                }
                if (std::ranges::find(binding.sequences, *sequence) != binding.sequences.end()) {
                    return corrupt(lineNumber, pieceStart + 1, std::format("duplicate sequence '{}'", piece));
                }
                binding.sequences.push_back(*sequence);
                if (bar == std::string_view::npos) {
                    break;
                }
                pos = bar + 1;
            }
        }
        keymap.m_sections[static_cast<std::size_t>(section)].push_back(std::move(binding));
    }

    if (!haveHeader) {
        return corrupt(lineNumber == 0 ? 1 : lineNumber, 1, "missing the header 'frost-keymap 1'");
    }
    if (!haveId || !haveName) {
        return corrupt(lineNumber, 1, haveId ? "missing name directive" : "missing id directive");
    }
    return keymap;
}

std::string formatKeymap(const Keymap& keymap) {
    std::string out = "frost-keymap 1\nid ";
    out += keymap.id();
    out += "\nname ";
    out += keymap.name();
    out += '\n';
    for (std::size_t s = 0; s < kSectionNames.size(); ++s) {
        const auto section = static_cast<KeymapSection>(s);
        if (!keymap.hasSection(section)) {
            continue;
        }
        out += "\n[";
        out += kSectionNames[s];
        out += "]\n";
        for (const Binding& binding : keymap.section(section)) {
            out += binding.actionId;
            out += " =";
            for (std::size_t i = 0; i < binding.sequences.size(); ++i) {
                out += i == 0 ? " " : " | ";
                out += formatSequence(binding.sequences[i]);
            }
            out += '\n';
        }
    }
    return out;
}

std::vector<Binding> Keymap::bindings(KeymapPlatform platform) const {
    const auto platformSection = platform == KeymapPlatform::Windows ? KeymapSection::Windows
                                 : platform == KeymapPlatform::Linux ? KeymapSection::Linux
                                                                     : KeymapSection::MacOS;
    const std::span<const Binding> defaults = section(KeymapSection::Default);
    const std::span<const Binding> specific = section(platformSection);

    // The platform section's actions, sorted, with their positions.
    std::vector<std::pair<std::string_view, std::size_t>> byId;
    byId.reserve(specific.size());
    for (std::size_t i = 0; i < specific.size(); ++i) {
        byId.emplace_back(specific[i].actionId, i);
    }
    std::ranges::sort(byId);
    const auto lookup = [&](std::string_view id) -> const Binding* {
        const auto it = std::ranges::lower_bound(byId, id, {}, &std::pair<std::string_view, std::size_t>::first);
        return it != byId.end() && it->first == id ? &specific[it->second] : nullptr;
    };

    std::vector<Binding> resolved;
    resolved.reserve(defaults.size() + specific.size());
    std::vector<std::string_view> defaultIds;
    defaultIds.reserve(defaults.size());
    for (const Binding& binding : defaults) {
        defaultIds.push_back(binding.actionId);
        if (const Binding* override = lookup(binding.actionId)) {
            resolved.push_back(*override);
        } else if (platform == KeymapPlatform::MacOS) {
            Binding derived{binding.actionId, {}};
            derived.sequences.reserve(binding.sequences.size());
            for (const Sequence& s : binding.sequences) {
                derived.sequences.push_back(swapCtrlMeta(s));
            }
            resolved.push_back(std::move(derived));
        } else {
            resolved.push_back(binding);
        }
    }
    std::ranges::sort(defaultIds);
    for (const Binding& binding : specific) {
        if (!std::ranges::binary_search(defaultIds, std::string_view(binding.actionId))) {
            resolved.push_back(binding);
        }
    }
    return resolved;
}

Result<Keymap> builtinKeymap(std::string_view id) {
    const std::string_view text = builtinKeymapText(id);
    if (text.empty()) {
        return fail(ErrorCode::NotFound, std::format("no built-in keymap '{}'", id));
    }
    auto keymap = parseKeymap(text);
    if (!keymap) {
        return fail(ErrorCode::Internal,
                    std::format("built-in keymap '{}' does not parse: {}", id, keymap.error().message()));
    }
    return keymap;
}

namespace {

// The first problem with one override, or nullopt when it is valid.
std::optional<std::string> overrideProblem(const ActionOverride& entry) {
    if (!isValidActionId(entry.actionId)) {
        return std::format("override for invalid action id '{}'", entry.actionId);
    }
    if (entry.sequences.size() > kMaxSequencesPerOverride) {
        return std::format("override for '{}' has {} sequences; the limit is {}", entry.actionId,
                           entry.sequences.size(), kMaxSequencesPerOverride);
    }
    for (std::size_t i = 0; i < entry.sequences.size(); ++i) {
        const Sequence& s = entry.sequences[i];
        if (!isStrictValid(s)) {
            return std::format("override for '{}' has an invalid sequence '{}'", entry.actionId, formatSequence(s));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (entry.sequences[j] == s) {
                return std::format("override for '{}' repeats '{}'", entry.actionId, formatSequence(s));
            }
        }
    }
    return std::nullopt;
}

bool isBuiltinKeymapId(std::string_view id) noexcept {
    return std::ranges::find(builtinKeymapIds(), id) != builtinKeymapIds().end();
}

bool isValidInterval(IClock::Duration interval) noexcept {
    return interval >= IClock::Duration::zero() && interval <= kMaxDoubleTapInterval;
}

} // namespace

Result<void> validate(const KeymapPreferences& preferences) {
    if (!isBuiltinKeymapId(preferences.keymapId)) {
        return fail(ErrorCode::InvalidArgument, std::format("unknown keymap id '{}'", preferences.keymapId));
    }
    if (!isValidInterval(preferences.doubleTapInterval)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("double-tap interval {} ms is outside [0, 2000] ms",
                        std::chrono::duration_cast<std::chrono::milliseconds>(preferences.doubleTapInterval).count()));
    }
    if (preferences.overrides.size() > kMaxOverrides) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} overrides; the limit is {}", preferences.overrides.size(), kMaxOverrides));
    }
    std::vector<std::string_view> ids;
    ids.reserve(preferences.overrides.size());
    for (const ActionOverride& entry : preferences.overrides) {
        if (auto problem = overrideProblem(entry)) {
            return fail(ErrorCode::InvalidArgument, std::move(*problem));
        }
        ids.push_back(entry.actionId);
    }
    std::ranges::sort(ids);
    if (const auto dup = std::ranges::adjacent_find(ids); dup != ids.end()) {
        return fail(ErrorCode::InvalidArgument, std::format("two overrides for '{}'", *dup));
    }
    return {};
}

KeymapPreferences sanitize(const KeymapPreferences& preferences, std::optional<Error>& firstProblem) {
    bool found = false;
    const auto note = [&](std::string message) {
        if (!found) {
            firstProblem.emplace(ErrorCode::InvalidArgument, std::move(message));
            found = true;
        }
    };
    KeymapPreferences out;
    if (isBuiltinKeymapId(preferences.keymapId)) {
        out.keymapId = preferences.keymapId;
    } else {
        note(std::format("unknown keymap id '{}'; using intellij", preferences.keymapId));
    }
    if (isValidInterval(preferences.doubleTapInterval)) {
        out.doubleTapInterval = preferences.doubleTapInterval;
    } else {
        note("double-tap interval outside [0, 2000] ms; using the platform interval");
    }
    std::vector<std::string_view> kept; // Sorted ids of the overrides kept so far.
    for (const ActionOverride& entry : preferences.overrides) {
        if (auto problem = overrideProblem(entry)) {
            note(std::move(*problem));
            continue;
        }
        const auto at = std::ranges::lower_bound(kept, std::string_view(entry.actionId));
        if (at != kept.end() && *at == entry.actionId) {
            note(std::format("two overrides for '{}'; the first is kept", entry.actionId));
            continue;
        }
        if (out.overrides.size() == kMaxOverrides) {
            note(std::format("more than {} overrides; the rest are dropped", kMaxOverrides));
            break;
        }
        kept.insert(at, entry.actionId);
        out.overrides.push_back(entry);
    }
    return out;
}

} // namespace frost::input
