// SPDX-License-Identifier: Apache-2.0
//
// The .theme.json compiler (frost/ui/theme_compiler/theme_compiler.hpp documents the format
// and the resolution rules): input limits, the duplicate-member pre-scan, QJsonDocument parsing,
// flattening of nested and dotted "ui" keys, registry-order resolution and validation.

#include "frost/ui/theme_compiler/theme_compiler.hpp"

#include "frost/io/utf.hpp"
#include "frost/ui/model/theme_blob.hpp"
#include "frost/ui/model/theme_registry.hpp"

#include "json_prescan.hpp"
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <tuple>

namespace frost::ui {

namespace {

constexpr std::string_view kRoot = "$";

// The longest colour or metric key name: a "ui" key longer than this matches nothing.
constexpr std::size_t kLongestKeyName = [] {
    std::size_t longest = 0;
    for (const ColorKeyInfo& entry : kColorKeys) {
        longest = std::max(longest, entry.name.size());
    }
    for (const MetricKeyInfo& entry : kMetricKeys) {
        longest = std::max(longest, entry.name.size());
    }
    return longest;
}();

// The last dot-separated segment of a registry key name ("Button.default.background" ->
// "background"), which a "*" wildcard property matches.
std::string_view lastSegment(std::string_view name) noexcept {
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

std::string utf8(const QString& text) { return text.toStdString(); }

std::string_view typeName(const QJsonValue& value) noexcept {
    switch (value.type()) {
    case QJsonValue::Null: return "null";
    case QJsonValue::Bool: return "a bool";
    case QJsonValue::Double: return "a number";
    case QJsonValue::String: return "a string";
    case QJsonValue::Array: return "an array";
    case QJsonValue::Object: return "an object";
    case QJsonValue::Undefined: break;
    }
    return "undefined";
}

struct Leaf {
    QJsonValue value;
    std::string path;
};

// How a key resolved: the value, the leaf it came from (for diagnostics) and, for metrics, the
// key whose explicit or wildcard value it is (the fallback target), which decides its compact
// value.
template<class T, class Key>
struct Resolved {
    bool ok = false;
    T value{};
    std::string path;
    Key source{};
};

class Compiler {
public:
    Compiler(std::string_view themeId, const CompileOptions& options) : m_themeId(themeId), m_options(options) {}

    CompileResult run(std::span<const std::byte> json) {
        compile(json);
        std::ranges::sort(m_diagnostics, [](const Diagnostic& a, const Diagnostic& b) {
            return std::tie(a.jsonPath, a.message, a.severity) < std::tie(b.jsonPath, b.message, b.severity);
        });
        m_diagnostics.erase(std::unique(m_diagnostics.begin(), m_diagnostics.end()), m_diagnostics.end());
        CompileResult result;
        const bool failed =
            std::ranges::any_of(m_diagnostics, [](const Diagnostic& d) { return d.severity == Diagnostic::Error; });
        if (!failed) {
            result.data = std::move(m_data);
        }
        result.diagnostics = std::move(m_diagnostics);
        return result;
    }

private:
    // Every diagnostic is bounded (kMaxDiagnosticPathBytes, kMaxDiagnosticMessageBytes).
    void add(Diagnostic::Severity severity, std::string_view path, std::string_view message) {
        m_diagnostics.push_back({severity, detail::elideForDiagnostic(path, kMaxDiagnosticPathBytes),
                                 detail::elideForDiagnostic(message, kMaxDiagnosticMessageBytes)});
    }
    void error(std::string_view path, std::string_view message) { add(Diagnostic::Error, path, message); }
    void warning(std::string_view path, std::string_view message) { add(Diagnostic::Warning, path, message); }
    // Unknown fields and keys: errors in strict mode, warnings in lenient mode.
    void unknown(std::string_view path, std::string_view message) {
        add(m_options.strict ? Diagnostic::Error : Diagnostic::Warning, path, message);
    }

    void compile(std::span<const std::byte> json) {
        if (!isValidThemeId(m_themeId)) {
            error(std::string(kRoot),
                  std::format("invalid theme id '{}' (expected [a-z0-9][a-z0-9-]{{0,63}})", m_themeId));
            return;
        }
        if (json.size() > kMaxThemeJsonBytes) {
            error(std::string(kRoot),
                  std::format("{} bytes is over the {}-byte theme file limit", json.size(), kMaxThemeJsonBytes));
            return;
        }
        const std::string_view text(reinterpret_cast<const char*>(json.data()), json.size());
        if (text.starts_with("\xEF\xBB\xBF")) {
            error(std::string(kRoot), "a UTF-8 byte order mark is not allowed");
            return;
        }
        if (!io::isValidUtf8(text)) {
            error(std::string(kRoot), "the file is not valid UTF-8");
            return;
        }
        const detail::PrescanResult scan = detail::prescanJson(text, kMaxThemeJsonDepth, kMaxReportedDuplicates);
        if (scan.depthExceeded) {
            error(std::string(kRoot), std::format("nesting is deeper than {} levels", kMaxThemeJsonDepth));
            return;
        }
        for (const detail::DuplicateMember& duplicate : scan.duplicates) {
            unknown(duplicate.jsonPath, "duplicate member name in one object");
        }
        if (scan.duplicateCount > scan.duplicates.size()) {
            unknown(kRoot, std::format("{} more duplicate member names are not listed",
                                       scan.duplicateCount - scan.duplicates.size()));
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray::fromRawData(text.data(), static_cast<qsizetype>(text.size())), &parseError);
        if (parseError.error != QJsonParseError::NoError || text.empty()) {
            error(std::string(kRoot), text.empty() ? std::string("malformed JSON: the file is empty")
                                                   : std::format("malformed JSON at offset {}: {}", parseError.offset,
                                                                 utf8(parseError.errorString())));
            return;
        }
        if (!document.isObject()) {
            error(std::string(kRoot), "the top level must be an object");
            return;
        }
        m_data.id = std::string(m_themeId);
        topLevel(document.object());
        resolveColors();
        resolveMetrics();
        reportUnusedKeys();
        checkUnderlays();
        if (std::ranges::none_of(m_diagnostics, [](const Diagnostic& d) { return d.severity == Diagnostic::Error; })) {
            if (auto valid = Theme::fromData(m_data); !valid) {
                error(std::string(kRoot), valid.error().message());
            }
        }
    }

    void topLevel(const QJsonObject& root) {
        bool haveName = false;
        bool haveDark = false;
        QJsonObject ui, icons, compact;
        // "colors" first: ui, icons and compact values may name them.
        if (const auto colors = root.value(QStringLiteral("colors")); !colors.isUndefined()) {
            if (colors.isObject()) {
                namedColors(colors.toObject());
            } else {
                error("colors", std::format("expected an object, found {}", typeName(colors)));
            }
        }
        for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
            const std::string key = utf8(it.key());
            const QJsonValue value = it.value();
            if (key == "name") {
                haveName = true;
                if (!value.isString()) {
                    error("name", std::format("expected a string, found {}", typeName(value)));
                    continue;
                }
                m_data.name = utf8(value.toString());
                if (m_data.name.empty() || m_data.name.size() > kMaxThemeNameBytes || !io::isValidUtf8(m_data.name)) {
                    error("name", std::format("the name must be 1..{} bytes of UTF-8", kMaxThemeNameBytes));
                }
            } else if (key == "dark" || key == "highContrast") {
                haveDark = haveDark || key == "dark";
                if (!value.isBool()) {
                    error(key, std::format("expected a bool, found {}", typeName(value)));
                } else if (key == "dark") {
                    m_data.dark = value.toBool();
                } else {
                    m_data.highContrast = value.toBool();
                }
            } else if (key == "author" || key == "$schema" || key == "colors") {
                // Ignored, or handled above.
            } else if (key == "ui" || key == "icons" || key == "compact") {
                if (!value.isObject()) {
                    error(key, std::format("expected an object, found {}", typeName(value)));
                } else if (key == "ui") {
                    ui = value.toObject();
                } else if (key == "icons") {
                    icons = value.toObject();
                } else {
                    compact = value.toObject();
                }
            } else {
                unknown(key, "unknown top-level field");
            }
        }
        if (!haveName) {
            error("name", "the required field \"name\" is missing");
        }
        if (!haveDark) {
            error("dark", "the required field \"dark\" is missing");
        }
        flattenUi(ui, "", "ui");
        m_compact = compact;
        iconPalette(icons);
    }

    void namedColors(const QJsonObject& colors) {
        // Two passes: every literal first, so a name that references another name is reported
        // as such rather than as an unknown colour.
        std::set<std::string> names;
        for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
            names.insert(utf8(it.key()));
        }
        for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
            const std::string name = utf8(it.key());
            const std::string path = "colors." + name;
            const QJsonValue value = it.value();
            if (!value.isString()) {
                error(path, std::format("expected a colour string, found {}", typeName(value)));
                continue;
            }
            const std::string literal = utf8(value.toString());
            if (const auto color = parseHexColor(literal)) {
                m_named.emplace(name, *color);
            } else if (names.contains(literal)) {
                error(path, std::format("a named colour may not reference another name ('{}')", literal));
            } else {
                error(path, std::format("invalid colour literal '{}' (expected #RRGGBB or #RRGGBBAA)", literal));
            }
            m_declaredNames.insert(name);
        }
    }

    // Collects the leaves of "ui" under their full dotted key names.
    void flattenUi(const QJsonObject& object, const std::string& keyPrefix, const std::string& pathPrefix) {
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const std::string name = utf8(it.key());
            const std::string path = pathPrefix + "." + name;
            const QJsonValue value = it.value();
            if (name == "*") {
                if (!keyPrefix.empty()) {
                    error(path, "a wildcard is allowed only directly under \"ui\"");
                } else if (!value.isObject()) {
                    error(path, std::format("a wildcard must be an object of properties, found {}", typeName(value)));
                } else {
                    wildcard(value.toObject(), path);
                }
                continue;
            }
            const std::string key = keyPrefix.empty() ? name : keyPrefix + "." + name;
            if (key.size() > kLongestKeyName) {
                /*
                 * Neither this key nor any below it can be a registry key. Reporting the member
                 * once, without descending, keeps the work per member at O(kLongestKeyName +
                 * name): keyPrefix is never longer than kLongestKeyName, so a long name shared
                 * by many members is never copied into each of their keys and paths.
                 */
                unknown(path,
                        std::format("unknown key {}{}", key, value.isObject() ? " (and every key under it)" : ""));
                continue;
            }
            if (value.isObject()) {
                flattenUi(value.toObject(), key, path);
                continue;
            }
            addLeaf(m_explicit, key, Leaf{value, path});
        }
    }

    void wildcard(const QJsonObject& object, const std::string& pathPrefix) {
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const std::string prop = utf8(it.key());
            const std::string path = pathPrefix + "." + prop;
            if (it.value().isObject() || it.value().isArray()) {
                error(path, "a wildcard applies to leaf properties only");
                continue;
            }
            addLeaf(m_wildcard, prop, Leaf{it.value(), path});
        }
    }

    void addLeaf(std::map<std::string, Leaf>& leaves, const std::string& key, Leaf leaf) {
        const auto [it, inserted] = leaves.try_emplace(key, leaf);
        if (inserted) {
            return;
        }
        // QJsonObject iterates in key order, so which spelling comes first is deterministic.
        if (it->second.value == leaf.value) {
            warning(leaf.path, std::format("{} is also set at {} (same value)", key, it->second.path));
        } else {
            error(leaf.path, std::format("{} is also set at {} with a different value", key, it->second.path));
        }
    }

    // A colour value: a literal or the name of a "colors" entry.
    std::optional<Rgba> colorValue(const Leaf& leaf) {
        if (!leaf.value.isString()) {
            error(leaf.path, std::format("expected a colour string, found {}", typeName(leaf.value)));
            return std::nullopt;
        }
        const std::string text = utf8(leaf.value.toString());
        if (const auto color = parseHexColor(text)) {
            return color;
        }
        if (text.starts_with('#')) {
            error(leaf.path, std::format("invalid colour literal '{}' (expected #RRGGBB or #RRGGBBAA)", text));
            return std::nullopt;
        }
        if (const auto it = m_named.find(text); it != m_named.end()) {
            return it->second;
        }
        if (!m_declaredNames.contains(text)) {
            error(leaf.path, std::format("unknown named colour '{}'", text));
        }
        return std::nullopt; // A declared but invalid name: reported at its declaration.
    }

    std::optional<float> metricValue(const Leaf& leaf, const MetricKeyInfo& key) {
        if (!leaf.value.isDouble()) {
            error(leaf.path, std::format("expected a number, found {}", typeName(leaf.value)));
            return std::nullopt;
        }
        const double value = leaf.value.toDouble();
        if (!std::isfinite(value) || value < key.min || value > key.max) {
            error(leaf.path, std::format("{} = {} is outside [{}, {}]", key.name, value, key.min, key.max));
            return std::nullopt;
        }
        return static_cast<float>(value);
    }

    // The explicit or wildcard leaf for a registry key name, marking it used.
    const Leaf* leafFor(std::string_view name) {
        if (const auto it = m_explicit.find(std::string(name)); it != m_explicit.end()) {
            m_usedExplicit.insert(it->first);
            return &it->second;
        }
        if (const auto it = m_wildcard.find(std::string(lastSegment(name))); it != m_wildcard.end()) {
            m_usedWildcard.insert(it->first);
            return &it->second;
        }
        return nullptr;
    }

    const Resolved<Rgba, ColorKey>& resolveColor(ColorKey key) {
        auto& slot = m_colors[static_cast<std::size_t>(key)];
        if (slot) {
            return *slot;
        }
        slot.emplace();
        const ColorKeyInfo& entry = info(key);
        if (const Leaf* leaf = leafFor(entry.name)) {
            if (const auto color = colorValue(*leaf)) {
                *slot = {true, *color, leaf->path, key};
            }
        } else if (entry.fallback != ColorKey::kCount) {
            const Resolved<Rgba, ColorKey> fallback = resolveColor(entry.fallback);
            m_colors[static_cast<std::size_t>(key)] = fallback;
        } else {
            error("ui." + std::string(entry.name), std::format("unresolved key {}", entry.name));
        }
        return *m_colors[static_cast<std::size_t>(key)];
    }

    void resolveColors() {
        for (const ColorKeyInfo& entry : kColorKeys) {
            const Resolved<Rgba, ColorKey>& resolved = resolveColor(entry.key);
            if (!resolved.ok) {
                continue;
            }
            if (entry.opaque && !resolved.value.opaque()) {
                error(resolved.path,
                      std::format("{} must be opaque, but resolves to {}", entry.name, resolved.value.toHex()));
            }
            m_data.colors[static_cast<std::size_t>(entry.key)] = resolved.value;
        }
    }

    const Resolved<float, MetricKey>& resolveMetric(MetricKey key) {
        auto& slot = m_metrics[static_cast<std::size_t>(key)];
        if (slot) {
            return *slot;
        }
        slot.emplace();
        const MetricKeyInfo& entry = info(key);
        if (const Leaf* leaf = leafFor(entry.name)) {
            if (const auto value = metricValue(*leaf, entry)) {
                *slot = {true, *value, leaf->path, key};
            }
        } else if (entry.fallback != MetricKey::kCount) {
            const Resolved<float, MetricKey> fallback = resolveMetric(entry.fallback);
            m_metrics[static_cast<std::size_t>(key)] = fallback;
        } else {
            error("ui." + std::string(entry.name), std::format("unresolved key {}", entry.name));
        }
        return *m_metrics[static_cast<std::size_t>(key)];
    }

    void resolveMetrics() {
        for (const MetricKeyInfo& entry : kMetricKeys) {
            const Resolved<float, MetricKey>& resolved = resolveMetric(entry.key);
            m_data.metrics[static_cast<std::size_t>(entry.key)] = resolved.value;
        }
        // Explicit compact values (only for keys with hasCompact).
        std::array<std::optional<float>, kMetricKeyCount> compact{};
        for (auto it = m_compact.constBegin(); it != m_compact.constEnd(); ++it) {
            const std::string name = utf8(it.key());
            const std::string path = "compact." + name;
            const auto key = metricKeyByName(name);
            if (!key) {
                unknown(path, std::format("unknown compact key {}", name));
                continue;
            }
            const MetricKeyInfo& entry = info(*key);
            if (!entry.hasCompact) {
                error(path, std::format("{} has no compact value", name));
                continue;
            }
            if (const auto value = metricValue(Leaf{it.value(), path}, entry)) {
                const float normal = m_data.metrics[static_cast<std::size_t>(*key)];
                if (entry.unit == MetricUnit::Px && *value > normal) {
                    error(path, std::format("compact {} = {} exceeds the normal value {}", name, *value, normal));
                }
                compact[static_cast<std::size_t>(*key)] = *value;
            }
        }
        // A key without its own compact value takes that of the key its normal value came from
        // (Tree.rowHeight follows List.rowHeight), else its normal value.
        for (const MetricKeyInfo& entry : kMetricKeys) {
            const auto index = static_cast<std::size_t>(entry.key);
            const Resolved<float, MetricKey>& resolved = resolveMetric(entry.key);
            std::optional<float> value = compact[index];
            if (!value && entry.hasCompact && resolved.ok && info(resolved.source).hasCompact) {
                value = compact[static_cast<std::size_t>(resolved.source)];
            }
            m_data.compactMetrics[index] = value.value_or(m_data.metrics[index]);
        }
    }

    void iconPalette(const QJsonObject& icons) {
        QJsonObject palette;
        for (auto it = icons.constBegin(); it != icons.constEnd(); ++it) {
            const std::string name = utf8(it.key());
            if (name != "ColorPalette") {
                unknown("icons." + name, "unknown icons field");
            } else if (!it.value().isObject()) {
                error("icons.ColorPalette", std::format("expected an object, found {}", typeName(it.value())));
            } else {
                palette = it.value().toObject();
            }
        }
        for (auto it = palette.constBegin(); it != palette.constEnd(); ++it) {
            const std::string name = utf8(it.key());
            if (!iconRoleByName(name)) {
                unknown("icons.ColorPalette." + name, std::format("{} is not an icon role (Icon.Default, ...)", name));
            }
        }
        for (const IconRoleInfo& role : kIconRoles) {
            const std::string path = "icons.ColorPalette." + std::string(role.name);
            const QJsonValue value =
                palette.value(QString::fromUtf8(role.name.data(), static_cast<qsizetype>(role.name.size())));
            if (value.isUndefined()) {
                error(path, std::format("the icon role {} is missing", role.name));
                continue;
            }
            if (const auto color = colorValue(Leaf{value, path})) {
                m_data.iconColors[static_cast<std::size_t>(role.role)] = *color;
            }
        }
    }

    void reportUnusedKeys() {
        for (const auto& [key, leaf] : m_explicit) {
            if (!m_usedExplicit.contains(key) && !colorKeyByName(key) && !metricKeyByName(key)) {
                unknown(leaf.path, std::format("unknown key {}", key));
            }
        }
        for (const auto& [prop, leaf] : m_wildcard) {
            if (m_usedWildcard.contains(prop)) {
                continue;
            }
            const bool matches =
                std::ranges::any_of(kColorKeys, [&](const ColorKeyInfo& k) { return lastSegment(k.name) == prop; }) ||
                std::ranges::any_of(kMetricKeys, [&](const MetricKeyInfo& k) { return lastSegment(k.name) == prop; });
            if (!matches) {
                unknown(leaf.path, std::format("the wildcard property {} matches no key", prop));
            }
        }
    }

    // A contrast pair whose background resolves translucent needs an opaque underlay. The
    // registry guarantees one for every background key that may be translucent; this guards
    // against a later registry or theme change that makes an "opaque" assumption false.
    void checkUnderlays() {
        for (const ContrastPair& pair : kContrastPairs) {
            const Rgba background = m_data.colors[static_cast<std::size_t>(pair.background)];
            if (!background.opaque() && pair.underlay == ColorKey::kCount) {
                error("ui." + std::string(info(pair.background).name),
                      std::format("{} is translucent but the contrast pair with {} has no underlay",
                                  info(pair.background).name, info(pair.foreground).name));
            }
        }
    }

    std::string_view m_themeId;
    const CompileOptions& m_options;
    ThemeData m_data;
    std::vector<Diagnostic> m_diagnostics;
    std::map<std::string, Rgba> m_named;
    std::set<std::string> m_declaredNames;
    std::map<std::string, Leaf> m_explicit;
    std::map<std::string, Leaf> m_wildcard;
    std::set<std::string> m_usedExplicit;
    std::set<std::string> m_usedWildcard;
    QJsonObject m_compact;
    std::array<std::optional<Resolved<Rgba, ColorKey>>, kColorKeyCount> m_colors{};
    std::array<std::optional<Resolved<float, MetricKey>>, kMetricKeyCount> m_metrics{};
};

} // namespace

CompileResult compileTheme(std::string_view themeId, std::span<const std::byte> json, const CompileOptions& options) {
    return Compiler(themeId, options).run(json);
}

Result<std::vector<std::byte>> compileThemeToBlob(std::string_view themeId, std::span<const std::byte> json,
                                                  const CompileOptions& options) {
    CompileResult result = compileTheme(themeId, json, options);
    if (!result.ok()) {
        std::string message = std::format("theme {} does not compile:", themeId);
        int shown = 0;
        for (const Diagnostic& d : result.diagnostics) {
            if (d.severity == Diagnostic::Error && shown < 3) {
                message += std::format("{} {}: {}", shown == 0 ? "" : ";", d.jsonPath, d.message);
                ++shown;
            }
        }
        return fail(ErrorCode::InvalidArgument, std::move(message));
    }
    return encodeThemeBlob(*result.data);
}

std::string formatDiagnostic(const Diagnostic& diagnostic) {
    return std::format("{}: {}: {}", diagnostic.jsonPath,
                       diagnostic.severity == Diagnostic::Error ? "error" : "warning", diagnostic.message);
}

} // namespace frost::ui
