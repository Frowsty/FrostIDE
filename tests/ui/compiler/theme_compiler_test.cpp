// SPDX-License-Identifier: Apache-2.0
//
// Tests of the .theme.json compiler (frost/ui/theme_compiler/theme_compiler.hpp): the shipped
// themes, key resolution precedence, value validation, input limits, determinism under member
// reordering, the frost-themec CLI (through realProcessLauncher) and the fuzz-harness replay.

#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/ui/model/contrast.hpp"
#include "frost/ui/model/theme_blob.hpp"
#include "frost/ui/theme_compiler/theme_compiler.hpp"

#include "fuzz/replay.hpp"
#include "fuzz/theme_json_harness.hpp"
#include "model/model_test_support.hpp"
#include "support/scratch_dir.hpp"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

const std::filesystem::path kSourceDir(FROST_SOURCE_DIR);
constexpr std::string_view kShippedThemes[] = {"glacier-dark",
                                               "glacier-light",
                                               "glacier-classic",
                                               "glacier-high-contrast",
                                               "rider-dark",
                                               "rider-light",
                                               "rider-islands-dark",
                                               "darcula",
                                               "monokai-pro",
                                               "dracula",
                                               "nord",
                                               "one-dark",
                                               "solarized-dark",
                                               "solarized-light",
                                               "catppuccin-mocha",
                                               "catppuccin-macchiato",
                                               "catppuccin-frappe",
                                               "catppuccin-latte",
                                               "tokyo-night",
                                               "tokyo-night-storm",
                                               "tokyo-night-light",
                                               "gruvbox-dark",
                                               "gruvbox-light",
                                               "github-dark",
                                               "github-light",
                                               "ayu-dark",
                                               "ayu-mirage",
                                               "ayu-light",
                                               "everforest-dark",
                                               "kanagawa-wave",
                                               "rose-pine",
                                               "rose-pine-dawn",
                                               "night-owl",
                                               "synthwave-84"};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << path.string();
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

CompileResult compileText(std::string_view text, CompileOptions options = {}, std::string_view id = "test-theme") {
    return compileTheme(id, test::asBytes(text), options);
}

std::string toJson(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

CompileResult compileObject(const QJsonObject& object, CompileOptions options = {}) {
    return compileText(toJson(object), options);
}

QString q(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// A valid theme with every key that has no fallback set explicitly (dotted form): colours
// #808080 (opaque) on a #202020 island, metrics at their minimum, icon roles black.
QJsonObject minimalTheme() {
    QJsonObject ui;
    for (const ColorKeyInfo& key : kColorKeys) {
        if (key.fallback == ColorKey::kCount) {
            ui.insert(q(key.name), QStringLiteral("#808080"));
        }
    }
    for (const MetricKeyInfo& key : kMetricKeys) {
        if (key.fallback == MetricKey::kCount) {
            ui.insert(q(key.name), static_cast<double>(key.min));
        }
    }
    QJsonObject palette;
    for (const IconRoleInfo& role : kIconRoles) {
        palette.insert(q(role.name), QStringLiteral("#000000"));
    }
    QJsonObject theme;
    theme.insert(QStringLiteral("name"), QStringLiteral("Test Theme"));
    theme.insert(QStringLiteral("dark"), true);
    theme.insert(QStringLiteral("ui"), ui);
    theme.insert(QStringLiteral("icons"), QJsonObject{{QStringLiteral("ColorPalette"), palette}});
    return theme;
}

// Applies `edit` to the "ui" object of `theme`.
template<class F>
QJsonObject withUi(QJsonObject theme, F edit) {
    QJsonObject ui = theme.value(QStringLiteral("ui")).toObject();
    edit(ui);
    theme.insert(QStringLiteral("ui"), ui);
    return theme;
}

bool has(const CompileResult& result, Diagnostic::Severity severity, std::string_view path, std::string_view text) {
    return std::ranges::any_of(result.diagnostics, [&](const Diagnostic& d) {
        return d.severity == severity && d.jsonPath == path && d.message.find(text) != std::string::npos;
    });
}

std::string dump(const CompileResult& result) {
    std::string out;
    for (const Diagnostic& d : result.diagnostics) {
        out += formatDiagnostic(d) + "\n";
    }
    return out;
}

Rgba color(const CompileResult& result, ColorKey key) { return result.data->colors[static_cast<std::size_t>(key)]; }

TEST(ThemeCompiler, MinimalThemeCompiles) {
    const CompileResult result = compileObject(minimalTheme());
    ASSERT_TRUE(result.ok()) << dump(result);
    EXPECT_TRUE(result.diagnostics.empty()) << dump(result);
    EXPECT_EQ(result.data->id, "test-theme");
    EXPECT_EQ(result.data->name, "Test Theme");
    EXPECT_TRUE(result.data->dark);
    EXPECT_FALSE(result.data->highContrast);
}

TEST(ThemeCompiler, ShippedThemesCompileStrict) {
    for (const std::string_view id : kShippedThemes) {
        const std::string json = readFile(kSourceDir / "resources" / "themes" / (std::string(id) + ".theme.json"));
        const CompileResult result = compileText(json, {}, id);
        ASSERT_TRUE(result.ok()) << id << "\n" << dump(result);
        EXPECT_TRUE(result.diagnostics.empty()) << id << "\n" << dump(result);
        for (const MetricKeyInfo& key : kMetricKeys) {
            const auto i = static_cast<std::size_t>(key.key);
            if (key.unit == MetricUnit::Px) {
                EXPECT_LE(result.data->compactMetrics[i], result.data->metrics[i]) << id << " " << key.name;
            }
        }
        EXPECT_EQ(result.data->highContrast, id == "glacier-high-contrast") << id;
        const bool isLight = (id == "glacier-light" || id == "rider-light" || id == "solarized-light" ||
                              id == "catppuccin-latte" || id == "tokyo-night-light" || id == "gruvbox-light" ||
                              id == "github-light" || id == "ayu-light" || id == "rose-pine-dawn");
        EXPECT_EQ(result.data->dark, !isLight) << id;
        ASSERT_TRUE(compileThemeToBlob(id, test::asBytes(json))) << id;
    }
    // Glacier Classic is flat: no island arc or gap in either density.
    const CompileResult classic = compileText(
        readFile(kSourceDir / "resources" / "themes" / "glacier-classic.theme.json"), {}, "glacier-classic");
    ASSERT_TRUE(classic.ok());
    for (const MetricKey key : {MetricKey::IslandArc, MetricKey::IslandGap}) {
        EXPECT_EQ(classic.data->metrics[static_cast<std::size_t>(key)], 0.0F);
        EXPECT_EQ(classic.data->compactMetrics[static_cast<std::size_t>(key)], 0.0F);
    }
}

// The declared contrast policy, checked at the compiler level for fast feedback; the screenshot
// tests check the same pairs on the installed blobs (ThemeContrast.DeclaredPairs).
TEST(ThemeCompiler, ShippedThemesMeetContrastPolicy) {
    for (const std::string_view id : kShippedThemes) {
        const CompileResult result =
            compileText(readFile(kSourceDir / "resources" / "themes" / (std::string(id) + ".theme.json")), {}, id);
        ASSERT_TRUE(result.ok()) << id;
        const Theme theme = *Theme::fromData(*result.data);
        for (const ContrastPair& pair : kContrastPairs) {
            if (const auto minimum = minimumContrast(pair.kind, theme.isHighContrast())) {
                EXPECT_GE(pairContrast(theme, pair), *minimum)
                    << id << ": " << info(pair.foreground).name << " on " << info(pair.background).name;
            }
        }
        for (const IconContrastPair& pair : kIconContrastPairs) {
            const double minimum = theme.isHighContrast() ? 7.0 : *minimumContrast(pair.kind, false);
            EXPECT_GE(pairContrast(theme, pair), minimum)
                << id << ": " << info(pair.role).name << " on " << info(pair.background).name;
        }
    }
}

TEST(ThemeCompiler, ExplicitBeatsWildcardBeatsFallback) {
    // TabbedPane.underlineColor falls back to Component.focusColor.
    const QJsonObject base = withUi(minimalTheme(), [](QJsonObject& ui) {
        ui.insert(QStringLiteral("Component.focusColor"), QStringLiteral("#112233"));
        ui.remove(QStringLiteral("Button.arc"));
    });
    QJsonObject theme =
        withUi(base, [](QJsonObject& ui) { ui.insert(QStringLiteral("*"), QJsonObject{{QStringLiteral("arc"), 3}}); });
    CompileResult result = compileObject(theme);
    ASSERT_TRUE(result.ok()) << dump(result);
    EXPECT_EQ(color(result, ColorKey::TabbedPaneUnderlineColor), Rgba{0xff112233U}) << "fallback";
    EXPECT_EQ(result.data->metrics[static_cast<std::size_t>(MetricKey::ButtonArc)], 3.0F) << "wildcard";
    EXPECT_EQ(result.data->metrics[static_cast<std::size_t>(MetricKey::IslandArc)], 0.0F) << "explicit beats wildcard";

    theme = withUi(theme, [](QJsonObject& ui) {
        ui.insert(QStringLiteral("*"), QJsonObject{{QStringLiteral("arc"), 3},
                                                   {QStringLiteral("underlineColor"), QStringLiteral("#445566")}});
    });
    result = compileObject(theme);
    ASSERT_TRUE(result.ok()) << dump(result);
    EXPECT_EQ(color(result, ColorKey::TabbedPaneUnderlineColor), Rgba{0xff445566U}) << "wildcard beats fallback";

    theme = withUi(theme, [](QJsonObject& ui) {
        ui.insert(QStringLiteral("TabbedPane"),
                  QJsonObject{{QStringLiteral("underlineColor"), QStringLiteral("#778899")}});
    });
    result = compileObject(theme);
    ASSERT_TRUE(result.ok()) << dump(result);
    EXPECT_EQ(color(result, ColorKey::TabbedPaneUnderlineColor), Rgba{0xff778899U}) << "explicit (nested) wins";
}

TEST(ThemeCompiler, NestedAndDottedSpellings) {
    // Three spellings of Button.default.background.
    QJsonObject theme = withUi(minimalTheme(), [](QJsonObject& ui) {
        ui.insert(QStringLiteral("Button"),
                  QJsonObject{{QStringLiteral("default"),
                               QJsonObject{{QStringLiteral("background"), QStringLiteral("#808080")}}}});
    });
    CompileResult result = compileObject(theme);
    ASSERT_TRUE(result.ok()) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Warning, "ui.Button.default.background", "same value")) << dump(result);

    theme = withUi(minimalTheme(), [](QJsonObject& ui) {
        ui.insert(QStringLiteral("Button"),
                  QJsonObject{{QStringLiteral("default.background"), QStringLiteral("#010203")}});
    });
    result = compileObject(theme);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has(result, Diagnostic::Error, "ui.Button.default.background", "different value")) << dump(result);
}

TEST(ThemeCompiler, LiteralDuplicateMembers) {
    std::string json = toJson(minimalTheme());
    // Insert a second "dark" member into the top-level object.
    json.insert(1, R"("dark":false,)");
    const CompileResult strict = compileText(json);
    EXPECT_FALSE(strict.ok());
    EXPECT_TRUE(has(strict, Diagnostic::Error, "dark", "duplicate member")) << dump(strict);
    const CompileResult lenient = compileText(json, CompileOptions{false});
    EXPECT_TRUE(lenient.ok()) << dump(lenient);
    EXPECT_TRUE(has(lenient, Diagnostic::Warning, "dark", "duplicate member")) << dump(lenient);
    // Escapes are decoded before names are compared.
    std::string escaped = toJson(minimalTheme());
    // "dark" with its d escaped (a backslash, then "u0064ark"), built from two literals so the
    // escape stays in the JSON text.
    escaped.insert(1, std::string("\"\\") + "u0064ark\":false,");
    ASSERT_EQ(escaped.substr(1, 11), std::string(1, '"') + std::string(1, '\\') + "u0064ark\"");
    EXPECT_TRUE(has(compileText(escaped), Diagnostic::Error, "dark", "duplicate member")) << dump(compileText(escaped));
    // Nested duplicates carry their path.
    const std::string nested = R"({"name":"x","dark":true,"ui":{"Island":{"arc":1,"arc":2}}})";
    EXPECT_TRUE(has(compileText(nested), Diagnostic::Error, "ui.Island.arc", "duplicate member"));
}

TEST(ThemeCompiler, DiagnosticsStayBoundedOnHostileInput) {
    /*
     * Every duplicate or unknown member below a long name used to copy that name into its own
     * diagnostic: output about the square of the input (gigabytes at the 1 MiB limit). Now
     * kMaxReportedDuplicates duplicates are listed, with elided paths, plus a count, and a ui
     * key longer than any registry key is reported once without descending.
     */
    const auto repeat = [](std::string_view unit, std::size_t count) {
        std::string out;
        out.reserve(unit.size() * count);
        for (std::size_t i = 0; i < count; ++i) {
            out += unit;
        }
        return out;
    };
    // `count` members all named "a".
    const auto duplicates = [&repeat](std::size_t count) { return "\"a\":1" + repeat(",\"a\":1", count - 1); };
    const auto expectBounded = [](const CompileResult& result) {
        EXPECT_FALSE(result.ok());
        EXPECT_LE(result.diagnostics.size(), kMaxReportedDuplicates + 4) << dump(result);
        std::size_t bytes = 0;
        for (const Diagnostic& d : result.diagnostics) {
            EXPECT_LE(d.jsonPath.size(), kMaxDiagnosticPathBytes + 3);
            EXPECT_LE(d.message.size(), kMaxDiagnosticMessageBytes + 3);
            bytes += d.jsonPath.size() + d.message.size();
        }
        EXPECT_LE(bytes, 64U * 1024U);
    };
    // The per-member duplicate diagnostics (equal ones are merged when the result is sorted).
    const auto listedDuplicates = [](const CompileResult& result) {
        return std::ranges::count_if(
            result.diagnostics, [](const Diagnostic& d) { return d.message == "duplicate member name in one object"; });
    };
    // A valid theme with `author` (any value is ignored) inserted first.
    const std::string good = toJson(minimalTheme());
    ASSERT_TRUE(compileText(good).ok());
    const auto withAuthor = [&good](const std::string& author) {
        std::string json = good;
        json.insert(1, "\"author\":" + author + ",");
        return json;
    };

    // 4,096 duplicates under a 16 KiB name: 32 listed (all at one path, so merged into one), 4,063 counted.
    const std::string longName = repeat("k", 16 * 1024);
    const CompileResult object = compileText(withAuthor("{\"" + longName + "\":{" + duplicates(4096) + "}}"));
    expectBounded(object);
    EXPECT_EQ(listedDuplicates(object), 1) << dump(object);
    EXPECT_TRUE(has(object, Diagnostic::Error, "$", "4063 more duplicate member names are not listed")) << dump(object);
    const auto elided = std::ranges::find_if(
        object.diagnostics, [](const Diagnostic& d) { return d.message == "duplicate member name in one object"; });
    ASSERT_NE(elided, object.diagnostics.end());
    EXPECT_TRUE(elided->jsonPath.starts_with("author.kkkk")) << elided->jsonPath;
    EXPECT_TRUE(elided->jsonPath.ends_with("kkk.a")) << elided->jsonPath;
    EXPECT_NE(elided->jsonPath.find("..."), std::string::npos);

    // The same through array elements: 2,000 objects of one duplicate each, whose distinct
    // paths ("author.kkk...kkk[i].a") keep the 32 listed apart.
    const CompileResult array =
        compileText(withAuthor("{\"" + longName + "\":[" + repeat(R"({"a":1,"a":2},)", 1999) + R"({"a":1,"a":2}]})"));
    expectBounded(array);
    EXPECT_EQ(listedDuplicates(array), static_cast<std::ptrdiff_t>(kMaxReportedDuplicates)) << dump(array);
    // The 32nd: the path's first and last 128 bytes around "...".
    EXPECT_TRUE(has(array, Diagnostic::Error,
                    "author." + longName.substr(0, 121) + "..." + longName.substr(0, 122) + "[31].a",
                    "duplicate member"))
        << dump(array);
    EXPECT_TRUE(has(array, Diagnostic::Error, "$", "1968 more duplicate member names are not listed")) << dump(array);

    // 2,048 distinct members under an unknown 8 KiB ui key: one diagnostic, not one each.
    QJsonObject inner;
    for (int i = 0; i < 2048; ++i) {
        inner.insert(QString::number(i), 1);
    }
    const CompileResult ui = compileObject(
        withUi(minimalTheme(), [&](QJsonObject& object) { object.insert(QString(8 * 1024, QChar('u')), inner); }));
    expectBounded(ui);
    const auto unknownKeys = std::ranges::count_if(
        ui.diagnostics, [](const Diagnostic& d) { return d.message.find("unknown key") != std::string::npos; });
    EXPECT_EQ(unknownKeys, 1) << dump(ui);
    EXPECT_TRUE(std::ranges::any_of(ui.diagnostics, [](const Diagnostic& d) {
        return d.message.find("(and every key under it)") != std::string::npos;
    })) << dump(ui);

    // At the 1 MiB limit: a 512 KiB name over as many duplicates as still fit.
    const std::string prefix = "{\"" + repeat("k", 512 * 1024) + "\":{\"a\":1";
    const std::size_t count = (kMaxThemeJsonBytes - withAuthor(prefix + "}}").size()) / 6;
    const std::string limit = withAuthor(prefix + repeat(",\"a\":1", count) + "}}");
    ASSERT_LE(limit.size(), kMaxThemeJsonBytes);
    const CompileResult huge = compileText(limit);
    expectBounded(huge);
    EXPECT_TRUE(has(huge, Diagnostic::Error, "$", std::format("{} more duplicate", count - kMaxReportedDuplicates)))
        << dump(huge);
}

TEST(ThemeCompiler, UnresolvedKeyNamesItsPath) {
    const CompileResult result = compileObject(
        withUi(minimalTheme(), [](QJsonObject& ui) { ui.remove(QStringLiteral("MainWindow.background")); }));
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has(result, Diagnostic::Error, "ui.MainWindow.background", "unresolved key MainWindow.background"))
        << dump(result);
    const CompileResult metric =
        compileObject(withUi(minimalTheme(), [](QJsonObject& ui) { ui.remove(QStringLiteral("List.rowHeight")); }));
    EXPECT_TRUE(has(metric, Diagnostic::Error, "ui.List.rowHeight", "unresolved key")) << dump(metric);
    EXPECT_FALSE(has(metric, Diagnostic::Error, "ui.Tree.rowHeight", "unresolved")) << "reported once, at the root";
}

TEST(ThemeCompiler, ColourValues) {
    QJsonObject theme = minimalTheme();
    theme.insert(QStringLiteral("colors"), QJsonObject{{QStringLiteral("accent"), QStringLiteral("#3870d8")},
                                                       {QStringLiteral("glass"), QStringLiteral("#FFFFFF80")},
                                                       {QStringLiteral("alias"), QStringLiteral("accent")},
                                                       {QStringLiteral("bad"), QStringLiteral("#12345")},
                                                       {QStringLiteral("number"), 7}});
    theme = withUi(theme, [](QJsonObject& ui) {
        ui.insert(QStringLiteral("Component.focusColor"), QStringLiteral("accent"));
        ui.insert(QStringLiteral("List.hoverBackground"), QStringLiteral("glass"));
        ui.insert(QStringLiteral("Label.foreground"), QStringLiteral("nope"));
        ui.insert(QStringLiteral("Label.secondaryForeground"), 12);
        ui.insert(QStringLiteral("Link.foreground"), QStringLiteral("#GGGGGG"));
        ui.insert(QStringLiteral("Island.background"), QStringLiteral("glass"));
        ui.insert(QStringLiteral("Separator.color"), QStringLiteral("alias"));
    });
    const CompileResult result = compileObject(theme);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has(result, Diagnostic::Error, "colors.alias", "may not reference another name")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "colors.bad", "invalid colour literal")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "colors.number", "expected a colour string")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "ui.Label.foreground", "unknown named colour 'nope'")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "ui.Label.secondaryForeground", "found a number")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "ui.Link.foreground", "invalid colour literal")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "ui.Island.background", "must be opaque")) << dump(result);
    EXPECT_FALSE(has(result, Diagnostic::Error, "ui.Separator.color", "")) << "reported at the declaration only";
    EXPECT_FALSE(has(result, Diagnostic::Error, "ui.List.hoverBackground", "")) << "translucent is fine there";

    // The valid part compiles: names, case-insensitive literals and alpha.
    QJsonObject good = minimalTheme();
    good.insert(QStringLiteral("colors"), QJsonObject{{QStringLiteral("accent"), QStringLiteral("#3870d8")},
                                                      {QStringLiteral("glass"), QStringLiteral("#FFFFFF80")}});
    good = withUi(good, [](QJsonObject& ui) {
        ui.insert(QStringLiteral("Component.focusColor"), QStringLiteral("accent"));
        ui.insert(QStringLiteral("List.hoverBackground"), QStringLiteral("glass"));
    });
    const CompileResult ok = compileObject(good);
    ASSERT_TRUE(ok.ok()) << dump(ok);
    EXPECT_EQ(color(ok, ColorKey::ComponentFocusColor), Rgba{0xff3870d8U});
    EXPECT_EQ(color(ok, ColorKey::ListHoverBackground), Rgba{0x80ffffffU});
    EXPECT_EQ(color(ok, ColorKey::TreeHoverBackground), Rgba{0x80ffffffU}) << "Tree.* falls back to List.*";
}

TEST(ThemeCompiler, MetricsAndCompact) {
    QJsonObject theme = withUi(minimalTheme(), [](QJsonObject& ui) {
        ui.insert(QStringLiteral("List.rowHeight"), 24);
        ui.insert(QStringLiteral("Button.height"), 28);
    });
    theme.insert(QStringLiteral("compact"),
                 QJsonObject{{QStringLiteral("List.rowHeight"), 20}, {QStringLiteral("Button.height"), 24}});
    CompileResult result = compileObject(theme);
    ASSERT_TRUE(result.ok()) << dump(result);
    const auto metric = [&](MetricKey key, bool compact) {
        const auto i = static_cast<std::size_t>(key);
        return compact ? result.data->compactMetrics[i] : result.data->metrics[i];
    };
    EXPECT_EQ(metric(MetricKey::ButtonHeight, true), 24.0F);
    EXPECT_EQ(metric(MetricKey::TreeRowHeight, false), 24.0F) << "Tree.rowHeight falls back to List.rowHeight";
    EXPECT_EQ(metric(MetricKey::TreeRowHeight, true), 20.0F) << "and so does its compact value";
    EXPECT_EQ(metric(MetricKey::TextFieldHeight, true), metric(MetricKey::TextFieldHeight, false))
        << "no compact value: normal";

    QJsonObject bad = withUi(minimalTheme(), [](QJsonObject& ui) {
        ui.insert(QStringLiteral("Button.height"), 65);
        ui.insert(QStringLiteral("Island.arc"), QStringLiteral("10"));
        ui.insert(QStringLiteral("Window.inactiveAlpha"), 1.5);
    });
    bad.insert(QStringLiteral("compact"), QJsonObject{{QStringLiteral("Component.arc"), 2},
                                                      {QStringLiteral("List.rowHeight"), 30},
                                                      {QStringLiteral("Nope.metric"), 3}});
    result = compileObject(bad);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(has(result, Diagnostic::Error, "ui.Button.height", "outside")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "ui.Island.arc", "expected a number")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "ui.Window.inactiveAlpha", "outside")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "compact.Component.arc", "has no compact value")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "compact.List.rowHeight", "exceeds the normal value")) << dump(result);
    EXPECT_TRUE(has(result, Diagnostic::Error, "compact.Nope.metric", "unknown compact key")) << dump(result);
    result = compileObject(bad, CompileOptions{false});
    EXPECT_TRUE(has(result, Diagnostic::Warning, "compact.Nope.metric", "unknown compact key")) << dump(result);
}

TEST(ThemeCompiler, UnknownKeysAndFields) {
    QJsonObject theme = withUi(minimalTheme(), [](QJsonObject& ui) {
        ui.insert(QStringLiteral("Button.backgroud"), QStringLiteral("#808080"));
        ui.insert(QStringLiteral("*"), QJsonObject{{QStringLiteral("nothingMatchesThis"), 1}});
    });
    theme.insert(QStringLiteral("parentTheme"), QStringLiteral("x"));
    theme.insert(QStringLiteral("author"), QStringLiteral("ignored"));
    theme.insert(QStringLiteral("$schema"), QStringLiteral("ignored"));
    QJsonObject icons = theme.value(QStringLiteral("icons")).toObject();
    QJsonObject palette = icons.value(QStringLiteral("ColorPalette")).toObject();
    palette.insert(QStringLiteral("Actions.Grey"), QStringLiteral("#808080"));
    icons.insert(QStringLiteral("ColorPalette"), palette);
    icons.insert(QStringLiteral("other"), 1);
    theme.insert(QStringLiteral("icons"), icons);
    const CompileResult strict = compileObject(theme);
    EXPECT_FALSE(strict.ok());
    EXPECT_TRUE(has(strict, Diagnostic::Error, "ui.Button.backgroud", "unknown key")) << dump(strict);
    EXPECT_TRUE(has(strict, Diagnostic::Error, "ui.*.nothingMatchesThis", "matches no key")) << dump(strict);
    EXPECT_TRUE(has(strict, Diagnostic::Error, "parentTheme", "unknown top-level field")) << dump(strict);
    EXPECT_TRUE(has(strict, Diagnostic::Error, "icons.ColorPalette.Actions.Grey", "not an icon role")) << dump(strict);
    EXPECT_TRUE(has(strict, Diagnostic::Error, "icons.other", "unknown icons field")) << dump(strict);
    const CompileResult lenient = compileObject(theme, CompileOptions{false});
    EXPECT_TRUE(lenient.ok()) << dump(lenient);
    EXPECT_EQ(lenient.diagnostics.size(), 5U) << dump(lenient);
    EXPECT_TRUE(std::ranges::all_of(lenient.diagnostics,
                                    [](const Diagnostic& d) { return d.severity == Diagnostic::Warning; }));
}

TEST(ThemeCompiler, StructuralErrors) {
    const auto expectError = [](const QJsonObject& theme, std::string_view path, std::string_view text) {
        const CompileResult result = compileObject(theme);
        EXPECT_FALSE(result.ok()) << path;
        EXPECT_TRUE(has(result, Diagnostic::Error, path, text)) << dump(result);
    };
    expectError(withUi(minimalTheme(),
                       [](QJsonObject& ui) {
                           ui.insert(QStringLiteral("*"), QJsonObject{{QStringLiteral("x"), QJsonObject{}}});
                       }),
                "ui.*.x", "leaf properties only");
    expectError(withUi(minimalTheme(),
                       [](QJsonObject& ui) {
                           ui.insert(QStringLiteral("Button"), QJsonObject{{QStringLiteral("*"), QJsonObject{}}});
                       }),
                "ui.Button.*", "only directly under");
    expectError(withUi(minimalTheme(), [](QJsonObject& ui) { ui.insert(QStringLiteral("*"), 4); }), "ui.*",
                "must be an object");
    QJsonObject theme = minimalTheme();
    theme.remove(QStringLiteral("name"));
    expectError(theme, "name", "is missing");
    theme = minimalTheme();
    theme.insert(QStringLiteral("name"), QString(65, QChar('n')));
    expectError(theme, "name", "1..64 bytes");
    theme.insert(QStringLiteral("name"), 3);
    expectError(theme, "name", "expected a string");
    theme = minimalTheme();
    theme.remove(QStringLiteral("dark"));
    expectError(theme, "dark", "is missing");
    theme = minimalTheme();
    theme.insert(QStringLiteral("highContrast"), QStringLiteral("yes"));
    expectError(theme, "highContrast", "expected a bool");
    theme = minimalTheme();
    theme.insert(QStringLiteral("ui"), QJsonArray{});
    expectError(theme, "ui", "expected an object");
    theme = minimalTheme();
    QJsonObject palette =
        theme.value(QStringLiteral("icons")).toObject().value(QStringLiteral("ColorPalette")).toObject();
    palette.remove(QStringLiteral("Icon.Warning"));
    theme.insert(QStringLiteral("icons"), QJsonObject{{QStringLiteral("ColorPalette"), palette}});
    expectError(theme, "icons.ColorPalette.Icon.Warning", "is missing");
    theme = minimalTheme();
    theme.insert(QStringLiteral("highContrast"), true);
    const CompileResult hc = compileObject(theme);
    ASSERT_TRUE(hc.ok());
    EXPECT_TRUE(hc.data->highContrast);
    EXPECT_FALSE(compileText(toJson(minimalTheme()), {}, "Bad_Id").ok()) << "the theme id is checked";
}

TEST(ThemeCompiler, InputLimits) {
    const std::string good = toJson(minimalTheme());
    ASSERT_TRUE(compileText(good).ok());
    const auto rootError = [](const CompileResult& result, std::string_view text) {
        return !result.ok() && has(result, Diagnostic::Error, "$", text);
    };
    std::string big = good;
    big.append(kMaxThemeJsonBytes + 1 - big.size(), ' ');
    EXPECT_TRUE(rootError(compileText(big), "limit")) << dump(compileText(big));
    std::string justFits = good;
    justFits.append(kMaxThemeJsonBytes - justFits.size(), ' ');
    EXPECT_TRUE(compileText(justFits).ok());
    EXPECT_TRUE(rootError(compileText("\xEF\xBB\xBF" + good), "byte order mark"));
    std::string badUtf8 = good;
    badUtf8.insert(badUtf8.find("Test Theme"), "\xC0\x80");
    EXPECT_TRUE(rootError(compileText(badUtf8), "not valid UTF-8"));
    EXPECT_TRUE(rootError(compileText(""), "empty"));
    EXPECT_TRUE(rootError(compileText("[]"), "must be an object"));
    EXPECT_TRUE(rootError(compileText("{\"name\": "), "malformed JSON"));
    // Depth: the top-level object is level 1, so "author" holding 7 nested objects reaches 8.
    const auto nested = [&](int levels) {
        std::string value = "1";
        for (int i = 0; i < levels; ++i) {
            value = "{\"a\":" + value + "}";
        }
        std::string json = good;
        json.insert(1, "\"author\":" + value + ",");
        return json;
    };
    EXPECT_TRUE(compileText(nested(7)).ok()) << dump(compileText(nested(7)));
    EXPECT_TRUE(rootError(compileText(nested(8)), "deeper than 8"));
    EXPECT_TRUE(rootError(compileText(nested(500)), "deeper than 8")) << "no deep recursion";
}

TEST(ThemeCompiler, MalformedJsonCorpus) {
    const std::filesystem::path dir = kSourceDir / "tests" / "ui" / "fixtures" / "themes" / "bad";
    std::size_t cases = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string json = readFile(entry.path());
        const CompileResult result = compileText(json);
        EXPECT_FALSE(result.ok()) << entry.path().filename().string();
        EXPECT_FALSE(result.diagnostics.empty()) << entry.path().filename().string();
        EXPECT_TRUE(std::ranges::any_of(result.diagnostics,
                                        [](const Diagnostic& d) { return d.severity == Diagnostic::Error; }));
        ++cases;
    }
    EXPECT_GE(cases, 25U);
}

// Serialises `value` with every object's members in a random order (QJsonDocument would sort them).
std::string shuffledJson(const QJsonValue& value, test::SplitMix64& rng) {
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        for (qsizetype i = keys.size() - 1; i > 0; --i) {
            keys.swapItemsAt(i, static_cast<qsizetype>(rng.below(static_cast<std::uint64_t>(i) + 1)));
        }
        std::string out = "{";
        for (const QString& key : keys) {
            if (out.size() > 1) {
                out += ",";
            }
            out += QJsonDocument(QJsonArray{key}).toJson(QJsonDocument::Compact).toStdString().substr(1);
            out.pop_back(); // "[\"key\"]" -> "\"key\""
            out += ":" + shuffledJson(object.value(key), rng);
        }
        return out + "}";
    }
    // Scalars and arrays: QJsonDocument's own encoding of a one-element array, unwrapped.
    std::string wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).toStdString();
    return wrapped.substr(1, wrapped.size() - 2);
}

TEST(ThemeCompiler, DeterministicUnderMemberShuffle) {
    // Consistency: QJsonObject orders keys itself, so this guards the compiler's own walk order.
    const std::string json = readFile(kSourceDir / "resources" / "themes" / "glacier-dark.theme.json");
    const auto reference = compileThemeToBlob("glacier-dark", test::asBytes(json));
    ASSERT_TRUE(reference) << reference.error().describe();
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    test::SplitMix64 rng(1234);
    for (int i = 0; i < 100; ++i) {
        const std::string shuffled = shuffledJson(document.object(), rng);
        const auto blob = compileThemeToBlob("glacier-dark", test::asBytes(shuffled));
        ASSERT_TRUE(blob) << blob.error().describe();
        ASSERT_EQ(*blob, *reference) << shuffled;
    }
}

io::ProcessResult runThemec(std::vector<std::string> arguments) {
    io::ProcessSpec spec;
    spec.executable = FROST_THEMEC;
    spec.arguments = std::move(arguments);
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, "", std::chrono::seconds(60));
    EXPECT_TRUE(result) << result.error().describe();
    return result.value_or(io::ProcessResult{});
}

TEST(ThemecCli, ExitCodesOutputsAndRestat) {
    frost::test::ScratchDir scratch("themec");
    const std::string out = scratch.join("out");
    ASSERT_TRUE(io::realFileSystem().createDirectories(out));
    const std::string good = scratch.join("good-theme.theme.json");
    const std::string bad = scratch.join("bad-theme.theme.json");
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(good, toJson(minimalTheme())));
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(bad, R"({"name": "Bad", "dark": true, "ui": {"Nope.key": 1}})"));

    io::ProcessResult result = runThemec({"--out-dir", out, good});
    EXPECT_EQ(result.exitCode, 0) << result.stderrBytes;
    const std::string blobPath = scratch.join("out/good-theme.ftheme");
    auto written = io::realFileSystem().readFile(blobPath);
    ASSERT_TRUE(written) << written.error().describe();
    const auto expected = compileThemeToBlob("good-theme", test::asBytes(toJson(minimalTheme())));
    ASSERT_TRUE(expected);
    EXPECT_EQ(*written, test::toString(*expected));

    // An identical re-run leaves the file alone (restat).
    const auto before = io::realFileSystem().status(blobPath);
    ASSERT_TRUE(before);
    result = runThemec({"--out-dir", out, good});
    EXPECT_EQ(result.exitCode, 0);
    const auto after = io::realFileSystem().status(blobPath);
    ASSERT_TRUE(after);
    EXPECT_EQ(before->mtime, after->mtime) << "rewritten although unchanged";

    // An error: exit 1, diagnostics in the documented format, nothing written for that input.
    result = runThemec({"--out-dir", out, bad, good});
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_NE(result.stderrBytes.find("bad-theme.theme.json: ui.Nope.key: error: unknown key Nope.key"),
              std::string::npos)
        << result.stderrBytes;
    EXPECT_FALSE(*io::realFileSystem().exists(scratch.join("out/bad-theme.ftheme")));
    // Lenient mode turns the unknown key into a warning, but unresolved keys still fail.
    EXPECT_EQ(runThemec({"--lenient", "--out-dir", out, bad}).exitCode, 1);

    // Usage errors.
    EXPECT_EQ(runThemec({}).exitCode, 2);
    EXPECT_EQ(runThemec({good}).exitCode, 2) << "no --out-dir";
    EXPECT_EQ(runThemec({"--out-dir"}).exitCode, 2);
    EXPECT_EQ(runThemec({"--out-dir", out}).exitCode, 2) << "no inputs";
    EXPECT_EQ(runThemec({"--frobnicate", "--out-dir", out, good}).exitCode, 2);
    // An unwritable (missing) output directory, a missing input and a badly named input.
    EXPECT_EQ(runThemec({"--out-dir", scratch.join("missing"), good}).exitCode, 1);
    EXPECT_EQ(runThemec({"--out-dir", out, scratch.join("absent.theme.json")}).exitCode, 1);
    const std::string misnamed = scratch.join("theme.json");
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(misnamed, toJson(minimalTheme())));
    EXPECT_EQ(runThemec({"--out-dir", out, misnamed}).exitCode, 1);
}

TEST(UiFuzzReplay, ThemeJson) {
    auto seeds = fuzz::readCorpus(kSourceDir / "tests" / "fuzz" / "corpus" / "theme_json");
    EXPECT_GE(seeds.size(), 4U) << "the shipped themes are committed as seeds";
    const std::string minimal = toJson(minimalTheme());
    seeds.emplace_back(minimal.begin(), minimal.end());
    fuzz::replay(fuzz::runThemeJsonInput, seeds, 2000, 0x75e3);
}

} // namespace
} // namespace frost::ui
