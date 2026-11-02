// SPDX-License-Identifier: Apache-2.0
//
// EditorConfig support (editorconfig.hpp): the .editorconfig reader, the section glob matcher, the
// upward file search and the property-to-key mapping. Written from the format description at
// editorconfig.org; no EditorConfig implementation was consulted.

#include "frost/settings/editorconfig.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <system_error>

namespace frost::settings {

namespace {

constexpr std::string_view kFileName = ".editorconfig";
constexpr std::size_t kMaxGlobDepth = 64; // Recursion bound for nested braces and stars.

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const std::size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return result;
}

[[nodiscard]] bool isKnownProperty(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 9> kKnown = {
        "indent_style",         "indent_size",     "tab_width", "end_of_line", "charset", "trim_trailing_whitespace",
        "insert_final_newline", "max_line_length", "root"};
    return std::ranges::find(kKnown, name) != kKnown.end();
}

[[nodiscard]] std::optional<std::int64_t> parseInteger(std::string_view text) noexcept {
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// --- Glob matching ---

// The index of the '}' that closes the '{' at `open`, or npos.
[[nodiscard]] std::size_t closingBrace(std::string_view pattern, std::size_t open) noexcept {
    std::size_t depth = 0;
    for (std::size_t i = open; i < pattern.size(); ++i) {
        if (pattern[i] == '\\') {
            ++i;
        } else if (pattern[i] == '{') {
            ++depth;
        } else if (pattern[i] == '}' && --depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

// Splits brace contents at commas outside nested braces.
[[nodiscard]] std::vector<std::string_view> alternatives(std::string_view body) {
    std::vector<std::string_view> result;
    std::size_t depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\') {
            ++i;
        } else if (body[i] == '{') {
            ++depth;
        } else if (body[i] == '}') {
            --depth;
        } else if (body[i] == ',' && depth == 0) {
            result.push_back(body.substr(start, i - start));
            start = i + 1;
        }
    }
    result.push_back(body.substr(start));
    return result;
}

// "{lo..hi}": both integers, possibly negative.
[[nodiscard]] std::optional<std::pair<std::int64_t, std::int64_t>> numericRange(std::string_view body) noexcept {
    const std::size_t dots = body.find("..");
    if (dots == std::string_view::npos) {
        return std::nullopt;
    }
    const auto low = parseInteger(body.substr(0, dots));
    const auto high = parseInteger(body.substr(dots + 2));
    if (!low || !high) {
        return std::nullopt;
    }
    return std::pair{std::min(*low, *high), std::max(*low, *high)};
}

[[nodiscard]] bool matchAt(std::string_view pattern, std::size_t pi, std::string_view text, std::size_t ti,
                           std::size_t depth) {
    if (depth > kMaxGlobDepth) {
        return false;
    }
    while (pi < pattern.size()) {
        char c = pattern[pi];
        if (c == '*') {
            const bool doubleStar = pi + 1 < pattern.size() && pattern[pi + 1] == '*';
            std::size_t next = pi + 1;
            while (next < pattern.size() && pattern[next] == '*') {
                ++next;
            }
            // "/**/" also matches a single '/', so "a/**/b" matches "a/b" (and "**/x" matches "x").
            if (doubleStar && next < pattern.size() && pattern[next] == '/' && (pi == 0 || pattern[pi - 1] == '/') &&
                matchAt(pattern, next + 1, text, ti, depth + 1)) {
                return true;
            }
            for (std::size_t k = ti;; ++k) {
                if (matchAt(pattern, next, text, k, depth + 1)) {
                    return true;
                }
                if (k >= text.size() || (!doubleStar && text[k] == '/')) {
                    return false;
                }
            }
        }
        if (c == '?') {
            if (ti >= text.size() || text[ti] == '/') {
                return false;
            }
            ++pi;
            ++ti;
            continue;
        }
        if (c == '[') {
            std::size_t j = pi + 1;
            const bool negate = j < pattern.size() && (pattern[j] == '!' || pattern[j] == '^');
            if (negate) {
                ++j;
            }
            const std::size_t first = j;
            std::size_t close = j < pattern.size() && pattern[j] == ']' ? j + 1 : j;
            while (close < pattern.size() && pattern[close] != ']') {
                close += pattern[close] == '\\' ? 2 : 1;
            }
            if (close < pattern.size()) {
                if (ti >= text.size() || text[ti] == '/') {
                    return false;
                }
                bool hit = false;
                for (std::size_t m = first; m < close;) {
                    char low = pattern[m];
                    if (low == '\\' && m + 1 < close) {
                        low = pattern[++m];
                    }
                    if (m + 2 < close && pattern[m + 1] == '-') {
                        const char high = pattern[m + 2];
                        hit = hit || (text[ti] >= low && text[ti] <= high);
                        m += 3;
                    } else {
                        hit = hit || text[ti] == low;
                        ++m;
                    }
                }
                if (hit == negate) {
                    return false;
                }
                pi = close + 1;
                ++ti;
                continue;
            }
            // No closing ']': a literal '['.
        }
        if (c == '{') {
            const std::size_t close = closingBrace(pattern, pi);
            if (close != std::string_view::npos) {
                const std::string_view body = pattern.substr(pi + 1, close - pi - 1);
                if (const auto range = numericRange(body)) {
                    std::size_t k = ti;
                    if (k < text.size() && text[k] == '-') {
                        ++k;
                    }
                    const std::size_t digits = k;
                    while (k < text.size() && text[k] >= '0' && text[k] <= '9') {
                        ++k;
                    }
                    const auto number = k > digits ? parseInteger(text.substr(ti, k - ti)) : std::nullopt;
                    return number && *number >= range->first && *number <= range->second &&
                           matchAt(pattern, close + 1, text, k, depth + 1);
                }
                const auto options = alternatives(body);
                if (options.size() > 1) {
                    const std::string_view rest = pattern.substr(close + 1);
                    return std::ranges::any_of(options, [&](std::string_view option) {
                        const std::string candidate = std::string(option) + std::string(rest);
                        return matchAt(candidate, 0, text, ti, depth + 1);
                    });
                }
            }
            // An unmatched '{' or "{single}" is literal.
        }
        if (c == '\\' && pi + 1 < pattern.size()) {
            c = pattern[++pi];
        }
        if (ti >= text.size() || text[ti] != c) {
            return false;
        }
        ++pi;
        ++ti;
    }
    return ti == text.size();
}

} // namespace

EditorConfigFile parseEditorConfig(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    EditorConfigFile file;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[') {
            const std::size_t close = line.rfind(']');
            if (close != std::string_view::npos && close > 0) {
                file.sections.push_back(
                    EditorConfigFile::Section{.glob = std::string(line.substr(1, close - 1)), .properties = {}});
            }
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string name = lower(trim(line.substr(0, equals)));
        std::string value(trim(line.substr(equals + 1)));
        if (name.empty()) {
            continue;
        }
        if (isKnownProperty(name) || lower(value) == "unset") {
            value = lower(value);
        }
        if (file.sections.empty()) {
            if (name == "root") {
                file.root = value == "true";
            }
            continue;
        }
        file.sections.back().properties.emplace_back(name, std::move(value));
    }
    return file;
}

bool editorConfigGlobMatches(std::string_view glob, std::string_view relativePath) {
    std::string pattern(glob);
    if (pattern.find('/') == std::string::npos) {
        pattern = "**/" + pattern;
    } else if (pattern.starts_with('/')) {
        pattern.erase(0, 1);
    }
    return matchAt(pattern, 0, relativePath, 0, 0);
}

const std::string* EditorConfigProperties::find(std::string_view name) const noexcept {
    const auto it =
        std::ranges::lower_bound(properties, name, {}, [](const auto& entry) { return std::string_view(entry.first); });
    return it != properties.end() && it->first == name ? &it->second : nullptr;
}

Result<EditorConfigProperties> resolveEditorConfig(io::IFileSystem& fs, std::string_view filePath, io::PathStyle style,
                                                   std::vector<Diagnostic>* diagnostics) {
    auto normalized = io::normalizePath(filePath, style);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    struct Found {
        std::string directory;
        std::string path;
        EditorConfigFile file;
    };
    std::vector<Found> found; // Innermost first.
    std::string directory(io::parentPath(*normalized, style));
    while (!directory.empty()) {
        auto path = io::joinPath(directory, kFileName, style);
        if (path) {
            auto text = fs.readFile(*path);
            if (text) {
                EditorConfigFile file = parseEditorConfig(*text);
                const bool root = file.root;
                found.push_back(Found{.directory = directory, .path = std::move(*path), .file = std::move(file)});
                if (root) {
                    break;
                }
            } else if (text.error().code() != ErrorCode::NotFound && diagnostics != nullptr) {
                diagnostics->push_back(Diagnostic{.severity = Severity::Warning,
                                                  .scope = Scope::FileLevel,
                                                  .path = *path,
                                                  .key = {},
                                                  .line = 0,
                                                  .column = 0,
                                                  .message = "cannot be read: " + text.error().describe()});
            }
        }
        const std::string_view parent = io::parentPath(directory, style);
        if (parent.empty() || parent == directory) {
            break;
        }
        directory = std::string(parent);
    }

    std::map<std::string, std::string, std::less<>> merged;
    EditorConfigProperties result;
    for (auto it = found.rbegin(); it != found.rend(); ++it) {
        std::string relative = normalized->substr(it->directory.size());
        std::ranges::replace(relative, '\\', '/');
        while (relative.starts_with('/')) {
            relative.erase(0, 1);
        }
        for (const EditorConfigFile::Section& section : it->file.sections) {
            if (!editorConfigGlobMatches(section.glob, relative)) {
                continue;
            }
            for (const auto& [name, value] : section.properties) {
                if (value == "unset") {
                    merged.erase(name);
                } else {
                    merged[name] = value;
                }
            }
        }
        result.files.push_back(it->path);
    }
    result.properties.assign(merged.begin(), merged.end());
    return result;
}

std::vector<std::pair<std::string, Value>> mapEditorConfig(const Registry& registry,
                                                           const EditorConfigProperties& properties,
                                                           std::vector<Diagnostic>* diagnostics) {
    std::map<std::string, Value, std::less<>> values;
    const auto skip = [&](std::string_view property, std::string_view value, std::string_view reason) {
        if (diagnostics != nullptr) {
            diagnostics->push_back(Diagnostic{.severity = Severity::Warning,
                                              .scope = Scope::FileLevel,
                                              .path = {},
                                              .key = std::string(property),
                                              .line = 0,
                                              .column = 0,
                                              .message = std::format("\"{}\" is ignored: {}", value, reason)});
        }
    };
    const auto put = [&](std::string_view property, std::string_view rawValue, std::string_view key, Value value) {
        const KeySpec* spec = registry.find(key);
        if (spec == nullptr || !spec->scopes.has(Scope::FileLevel)) {
            return;
        }
        if (auto ok = validateValue(spec->schema, value); !ok) {
            skip(property, rawValue, std::format("{} {}", key, ok.error().message()));
            return;
        }
        values.insert_or_assign(std::string(key), std::move(value));
    };

    if (const std::string* style = properties.find("indent_style")) {
        if (*style == "space" || *style == "tab") {
            put("indent_style", *style, key::editorInsertSpaces.name, Value(*style == "space"));
        } else {
            skip("indent_style", *style, "expected space or tab");
        }
    }
    const std::string* tabWidth = properties.find("tab_width");
    const std::string* indentSize = properties.find("indent_size");
    const auto tabWidthValue = tabWidth != nullptr ? parseInteger(*tabWidth) : std::nullopt;
    if (tabWidth != nullptr) {
        if (tabWidthValue) {
            put("tab_width", *tabWidth, key::editorTabSize.name, Value(*tabWidthValue));
        } else {
            skip("tab_width", *tabWidth, "expected a number");
        }
    }
    if (indentSize != nullptr) {
        if (*indentSize == "tab") {
            if (tabWidthValue) {
                put("indent_size", *indentSize, key::editorIndentSize.name, Value(*tabWidthValue));
            }
        } else if (const auto size = parseInteger(*indentSize)) {
            put("indent_size", *indentSize, key::editorIndentSize.name, Value(*size));
            if (tabWidth == nullptr) {
                put("indent_size", *indentSize, key::editorTabSize.name, Value(*size));
            }
        } else {
            skip("indent_size", *indentSize, "expected a number or tab");
        }
    }
    if (const std::string* eol = properties.find("end_of_line")) {
        if (*eol == "lf" || *eol == "crlf" || *eol == "cr") {
            put("end_of_line", *eol, key::filesLineSeparator.name, Value(*eol));
        } else {
            skip("end_of_line", *eol, "expected lf, crlf or cr");
        }
    }
    if (const std::string* charset = properties.find("charset")) {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kCharsets = {{
            {"latin1", "ISO-8859-1"},
            {"utf-8", "UTF-8"},
            {"utf-8-bom", "UTF-8"},
            {"utf-16be", "UTF-16BE"},
            {"utf-16le", "UTF-16LE"},
        }};
        const auto it = std::ranges::find_if(kCharsets, [&](const auto& entry) { return entry.first == *charset; });
        if (it != kCharsets.end()) {
            put("charset", *charset, key::filesEncoding.name, Value(std::string(it->second)));
            if (*charset == "utf-8" || *charset == "utf-8-bom") {
                put("charset", *charset, key::filesUtf8Bom.name, Value(*charset == "utf-8-bom"));
            }
        } else {
            skip("charset", *charset, "expected latin1, utf-8, utf-8-bom, utf-16be or utf-16le");
        }
    }
    if (const std::string* trimWhitespace = properties.find("trim_trailing_whitespace")) {
        if (*trimWhitespace == "true" || *trimWhitespace == "false") {
            put("trim_trailing_whitespace", *trimWhitespace, key::editorTrimTrailingWhitespace.name,
                Value(std::string(*trimWhitespace == "true" ? "all" : "none")));
        } else {
            skip("trim_trailing_whitespace", *trimWhitespace, "expected true or false");
        }
    }
    if (const std::string* finalNewline = properties.find("insert_final_newline")) {
        if (*finalNewline == "true" || *finalNewline == "false") {
            put("insert_final_newline", *finalNewline, key::editorInsertFinalNewline.name,
                Value(*finalNewline == "true"));
        } else {
            skip("insert_final_newline", *finalNewline, "expected true or false");
        }
    }
    if (const std::string* maxLength = properties.find("max_line_length")) {
        if (const auto length = parseInteger(*maxLength)) {
            put("max_line_length", *maxLength, key::editorRightMargin.name, Value(*length));
        } else if (*maxLength != "off") {
            skip("max_line_length", *maxLength, "expected a number or off");
        }
    }
    return {values.begin(), values.end()};
}

FileSettings::FileSettings(std::shared_ptr<const SettingsSnapshot> snapshot,
                           std::vector<std::pair<std::string, Value>> overlay)
    : m_snapshot(std::move(snapshot)), m_overlay(std::move(overlay)) {
    FROST_PRE(m_snapshot != nullptr);
    std::ranges::sort(m_overlay, {}, &std::pair<std::string, Value>::first);
}

const Value& FileSettings::value(std::string_view key) const {
    const auto it =
        std::ranges::lower_bound(m_overlay, key, {}, [](const auto& entry) { return std::string_view(entry.first); });
    if (it != m_overlay.end() && it->first == key) {
        return it->second;
    }
    return m_snapshot->value(key);
}

Scope FileSettings::source(std::string_view key) const {
    const auto it =
        std::ranges::lower_bound(m_overlay, key, {}, [](const auto& entry) { return std::string_view(entry.first); });
    if (it != m_overlay.end() && it->first == key) {
        return Scope::FileLevel;
    }
    return m_snapshot->source(key);
}

Result<FileSettings> settingsForFile(std::shared_ptr<const SettingsSnapshot> snapshot, io::IFileSystem& fs,
                                     std::string_view filePath, io::PathStyle style,
                                     std::vector<Diagnostic>* diagnostics) {
    FROST_PRE(snapshot != nullptr);
    if (!snapshot->get(key::editorEditorConfig)) {
        if (auto normalized = io::normalizePath(filePath, style); !normalized) {
            return std::unexpected(normalized.error());
        }
        return FileSettings(std::move(snapshot), {});
    }
    auto properties = resolveEditorConfig(fs, filePath, style, diagnostics);
    if (!properties) {
        return std::unexpected(properties.error());
    }
    auto overlay = mapEditorConfig(snapshot->registry(), *properties, diagnostics);
    return FileSettings(std::move(snapshot), std::move(overlay));
}

} // namespace frost::settings
