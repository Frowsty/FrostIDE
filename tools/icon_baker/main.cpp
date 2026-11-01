// SPDX-License-Identifier: Apache-2.0
//
// frost-icon-baker: bakes the icon manifest's SVGs into one FIA v1 atlas per scale at build time
// (ADR 0015 decision 7). Qt Svg is used here only; the IDE never loads an SVG at run time.
//
/*
 * Usage: frost-icon-baker --manifest <icons.json> --source-dir <dir> --out-dir <dir>
 *                         [--scales 100,125,150,175,200,250,300] [--depfile <file>]
 *
 * Manifest (schemaVersion 1):
 *   {"schemaVersion": 1, "icons": [{"id": "actions.search", "source": "lucide/search.svg",
 *     "sizes": [16, 20], "tint": "Icon.Default" | null, "strokePx": {"16": 1.0, "20": 1.25},
 *     "dark": "frost/x_dark.svg" (untinted icons only)}]}
 * Sources are relative to --source-dir, without ".." segments.
 *
 * Every scale is baked in memory first; then <out-dir>/icons-<pct>.fia is written for each
 * scale (QSaveFile, only when the bytes differ, so Ninja's restat prunes; Budget: B28). The
 * depfile names the first output and lists the manifest and every SVG read, spaces escaped.
 * Exit status: 0 ok; 1 on an invalid manifest, a missing, oversized or invalid SVG, any qt.svg
 * message, a <text> element, a cell too large for a page, or a failed write; 2 on a usage error.
 * The baker forces the offscreen platform and ignores the QT_* scaling variables, so its output
 * depends only on its inputs and its own binary (tested for byte-identical repeat bakes).
 */

#include "frost/ui/model/fnv1a.hpp"
#include "frost/ui/model/icon_atlas_format.hpp"
#include "frost/ui/model/theme_registry.hpp"

#include "no_dialogs.hpp"
#include "svg_raster.hpp"
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using frost::Result;

struct IconSpec {
    std::string id;
    QString source;
    std::vector<std::uint8_t> sizes;
    std::optional<frost::ui::IconRole> tint; // nullopt: an untinted full-colour icon.
    std::map<std::uint8_t, double> strokePx;
    std::optional<QString> dark;
};

void printLine(const std::string& line) { std::fprintf(stderr, "frost-icon-baker: %s\n", line.c_str()); }

int usage(const std::string& problem) {
    printLine(problem);
    std::fprintf(stderr, "usage: frost-icon-baker --manifest <icons.json> --source-dir <dir> --out-dir <dir> "
                         "[--scales 100,125,...] [--depfile <file>]\n");
    return 2;
}

// A relative source path without "..", "." or empty segments, or backslashes.
bool safeRelative(const QString& path) {
    if (path.isEmpty() || path.contains(QLatin1Char('\\')) || QDir::isAbsolutePath(path) ||
        path.contains(QLatin1Char(':'))) {
        return false;
    }
    for (const QString& segment : path.split(QLatin1Char('/'))) {
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String("..")) {
            return false;
        }
    }
    return true;
}

Result<std::vector<IconSpec>> parseManifest(const QByteArray& bytes) {
    const auto bad = [](std::string message) {
        return frost::fail(frost::ErrorCode::InvalidArgument, std::move(message));
    };
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return bad("the manifest is not a JSON object: " + parseError.errorString().toStdString());
    }
    const QJsonObject root = document.object();
    for (const QString& key : root.keys()) {
        if (key != QLatin1String("schemaVersion") && key != QLatin1String("icons")) {
            return bad("unknown manifest field " + key.toStdString());
        }
    }
    const QJsonValue schema = root.value(QLatin1String("schemaVersion"));
    if (!schema.isDouble() || schema.toDouble() != 1.0) {
        return bad("schemaVersion must be 1");
    }
    if (!root.value(QLatin1String("icons")).isArray()) {
        return bad("\"icons\" must be an array");
    }
    std::vector<IconSpec> icons;
    std::set<std::string> ids;
    for (const QJsonValue& value : root.value(QLatin1String("icons")).toArray()) {
        if (!value.isObject()) {
            return bad("every icon must be an object");
        }
        const QJsonObject object = value.toObject();
        IconSpec icon;
        icon.id = object.value(QLatin1String("id")).toString().toStdString();
        const std::string where = "icon " + icon.id + ": ";
        for (const QString& key : object.keys()) {
            static constexpr std::string_view kFields[] = {"id", "source", "sizes", "tint", "strokePx", "dark"};
            if (std::ranges::find(kFields, key.toStdString()) == std::end(kFields)) {
                return bad(where + "unknown field " + key.toStdString());
            }
        }
        if (!frost::ui::isValidIconId(icon.id) || !ids.insert(icon.id).second) {
            return bad("invalid or duplicate icon id '" + icon.id + "'");
        }
        icon.source = object.value(QLatin1String("source")).toString();
        if (!safeRelative(icon.source) || !icon.source.endsWith(QLatin1String(".svg"))) {
            return bad(where + "source must be a relative .svg path without '..'");
        }
        if (!object.value(QLatin1String("sizes")).isArray()) {
            return bad(where + "sizes must be an array");
        }
        for (const QJsonValue& size : object.value(QLatin1String("sizes")).toArray()) {
            const int px = size.toInt(-1);
            const bool known = std::ranges::find(frost::ui::kIconDesignSizes, px) != frost::ui::kIconDesignSizes.end();
            if (!size.isDouble() || !known || std::ranges::find(icon.sizes, px) != icon.sizes.end()) {
                return bad(where + "sizes must be distinct values of 14, 16 and 20");
            }
            icon.sizes.push_back(static_cast<std::uint8_t>(px));
        }
        if (icon.sizes.empty()) {
            return bad(where + "sizes is empty");
        }
        const QJsonValue tint = object.value(QLatin1String("tint"));
        if (tint.isString()) {
            icon.tint = frost::ui::iconRoleByName(tint.toString().toStdString());
            if (!icon.tint) {
                return bad(where + "tint must be an icon role (Icon.Default, ...) or null");
            }
        } else if (!tint.isNull()) {
            return bad(where + "tint must be an icon role name or null");
        }
        const QJsonValue stroke = object.value(QLatin1String("strokePx"));
        if (stroke.isObject()) {
            const QJsonObject strokes = stroke.toObject();
            for (const QString& key : strokes.keys()) {
                bool ok = false;
                const int size = key.toInt(&ok);
                const double px = strokes.value(key).toDouble(-1);
                if (!ok || std::ranges::find(icon.sizes, size) == icon.sizes.end() || !(px > 0.0 && px <= 8.0)) {
                    return bad(where + "strokePx maps baked sizes to widths in (0, 8]");
                }
                icon.strokePx[static_cast<std::uint8_t>(size)] = px;
            }
        } else if (!stroke.isUndefined()) {
            return bad(where + "strokePx must be an object");
        }
        const QJsonValue dark = object.value(QLatin1String("dark"));
        if (dark.isString()) {
            if (icon.tint) {
                return bad(where + "only untinted icons may have a dark variant");
            }
            if (!safeRelative(dark.toString()) || !dark.toString().endsWith(QLatin1String(".svg"))) {
                return bad(where + "dark must be a relative .svg path without '..'");
            }
            icon.dark = dark.toString();
        } else if (!dark.isUndefined()) {
            return bad(where + "dark must be a path");
        }
        icons.push_back(std::move(icon));
    }
    return icons;
}

Result<QByteArray> readSvg(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return frost::fail(frost::ErrorCode::NotFound, "cannot read " + QDir::toNativeSeparators(path).toStdString() +
                                                           ": " + file.errorString().toStdString());
    }
    QByteArray bytes = file.read(frost::tools::baker::kMaxSvgBytes + 1);
    if (bytes.size() > frost::tools::baker::kMaxSvgBytes) {
        return frost::fail(frost::ErrorCode::InvalidArgument,
                           QDir::toNativeSeparators(path).toStdString() + " is over the 64 KiB SVG limit");
    }
    return bytes;
}

bool writeIfChanged(const QString& path, const QByteArray& bytes) {
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.size() == bytes.size() && existing.readAll() == bytes) {
        return true;
    }
    existing.close();
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        printLine("cannot write " + QDir::toNativeSeparators(path).toStdString() + ": " +
                  out.errorString().toStdString());
        return false;
    }
    return true;
}

// Ninja depfile syntax: forward slashes, spaces (and '#', '$') escaped with a backslash.
std::string depfilePath(const QString& path) {
    std::string out;
    for (const char c : QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdString()) {
        if (c == ' ' || c == '#') {
            out += '\\';
        } else if (c == '$') {
            out += '$';
        }
        out += c;
    }
    return out;
}

struct Options {
    QString manifest, sourceDir, outDir, depfile;
    std::vector<std::uint16_t> scales{frost::ui::kAtlasScales.begin(), frost::ui::kAtlasScales.end()};
};

int bake(const Options& options) {
    QFile manifestFile(options.manifest);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        printLine("cannot read the manifest " + QDir::toNativeSeparators(options.manifest).toStdString());
        return 1;
    }
    const QByteArray manifest = manifestFile.readAll();
    auto icons = parseManifest(manifest);
    if (!icons) {
        printLine(QDir::toNativeSeparators(options.manifest).toStdString() + ": " + icons.error().message());
        return 1;
    }
    QStringList inputs{options.manifest};
    // sourceHash: the format version, then the manifest and every SVG, each length-prefixed.
    frost::ui::Fnv1a64 hash;
    hash.updateInteger(frost::ui::kIconAtlasVersion, 2);
    const auto mix = [&hash](const QByteArray& bytes) {
        hash.updateInteger(static_cast<std::uint64_t>(bytes.size()), 8);
        hash.update(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    };
    mix(manifest);
    struct Loaded {
        QByteArray svg;
        std::optional<QByteArray> dark;
    };
    std::vector<Loaded> sources;
    const QDir sourceDir(options.sourceDir);
    for (const IconSpec& icon : *icons) {
        Loaded loaded;
        const QString path = sourceDir.filePath(icon.source);
        inputs.push_back(path);
        auto svg = readSvg(path);
        if (!svg) {
            printLine("icon " + icon.id + ": " + svg.error().message());
            return 1;
        }
        loaded.svg = std::move(*svg);
        mix(loaded.svg);
        if (icon.dark) {
            const QString darkPath = sourceDir.filePath(*icon.dark);
            inputs.push_back(darkPath);
            auto dark = readSvg(darkPath);
            if (!dark) {
                printLine("icon " + icon.id + ": " + dark.error().message());
                return 1;
            }
            loaded.dark = std::move(*dark);
            mix(*loaded.dark);
        }
        sources.push_back(std::move(loaded));
    }

    // Prepare once per (icon, design size), render once per scale.
    std::map<std::uint16_t, QByteArray> atlases;
    for (const std::uint16_t scale : options.scales) {
        frost::ui::AtlasWriter writer(scale, hash.value());
        for (std::size_t i = 0; i < icons->size(); ++i) {
            const IconSpec& icon = (*icons)[i];
            for (const std::uint8_t size : icon.sizes) {
                const auto stroke =
                    icon.strokePx.contains(size) ? std::optional<double>(icon.strokePx.at(size)) : std::nullopt;
                const bool tinted = icon.tint.has_value();
                const std::string where = std::format("icon {}@{} ({}%): ", icon.id, size, scale);
                auto prepared = frost::tools::baker::prepareSvg(sources[i].svg, stroke, size, tinted);
                if (!prepared) {
                    printLine(where + prepared.error().message());
                    return 1;
                }
                const std::uint16_t devicePx = frost::ui::iconDevicePx(size, scale);
                auto cell = frost::tools::baker::rasterizeSvg(*prepared, devicePx, tinted);
                if (!cell) {
                    printLine(where + cell.error().message());
                    return 1;
                }
                std::optional<frost::ui::AtlasCell> darkCell;
                if (sources[i].dark) {
                    auto darkPrepared = frost::tools::baker::prepareSvg(*sources[i].dark, stroke, size, false);
                    if (!darkPrepared) {
                        printLine(where + "dark: " + darkPrepared.error().message());
                        return 1;
                    }
                    auto dark = frost::tools::baker::rasterizeSvg(*darkPrepared, devicePx, false);
                    if (!dark) {
                        printLine(where + "dark: " + dark.error().message());
                        return 1;
                    }
                    darkCell = std::move(*dark);
                }
                auto added = writer.add(icon.id, size, tinted, icon.tint.value_or(frost::ui::IconRole::Default),
                                        std::move(*cell), std::move(darkCell));
                if (!added) {
                    printLine(where + added.error().message());
                    return 1;
                }
            }
        }
        auto bytes = writer.finish();
        if (!bytes) {
            printLine(std::format("{}%: {}", scale, bytes.error().message()));
            return 1;
        }
        atlases[scale] =
            QByteArray(reinterpret_cast<const char*>(bytes->data()), static_cast<qsizetype>(bytes->size()));
    }

    const QDir outDir(options.outDir);
    if (!outDir.exists()) {
        printLine("the output directory " + QDir::toNativeSeparators(options.outDir).toStdString() + " does not exist");
        return 1;
    }
    QString firstOutput;
    for (const std::uint16_t scale : options.scales) {
        const QString path = outDir.filePath(QStringLiteral("icons-%1.fia").arg(scale));
        if (firstOutput.isEmpty()) {
            firstOutput = path;
        }
        if (!writeIfChanged(path, atlases[scale])) {
            return 1;
        }
    }
    if (!options.depfile.isEmpty()) {
        std::string text = depfilePath(firstOutput) + ":";
        for (const QString& input : inputs) {
            text += " " + depfilePath(input);
        }
        text += "\n";
        // Written on every run: Ninja consumes the depfile after each command.
        QSaveFile depfile(options.depfile);
        const QByteArray bytes = QByteArray::fromStdString(text);
        if (!depfile.open(QIODevice::WriteOnly) || depfile.write(bytes) != bytes.size() || !depfile.commit()) {
            printLine("cannot write the depfile " + QDir::toNativeSeparators(options.depfile).toStdString());
            return 1;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    frost::tools::disableToolDialogs();
    // Output must depend only on the inputs: no desktop platform, no user scaling.
    for (const char* name : {"QT_QPA_PLATFORM", "QT_SCALE_FACTOR", "QT_FONT_DPI", "QT_ENABLE_HIGHDPI_SCALING",
                             "QT_SCREEN_SCALE_FACTORS", "QT_SCALE_FACTOR_ROUNDING_POLICY"}) {
        qunsetenv(name);
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication::addLibraryPath(QStringLiteral(FROST_QT_PLUGIN_DIR));
    QStringList svgMessages;
    frost::tools::baker::installSvgMessageHandler(&svgMessages);
    QGuiApplication app(argc, argv);

    const QStringList args = QCoreApplication::arguments();
    Options options;
    for (qsizetype i = 1; i < args.size(); ++i) {
        const QString& arg = args[i];
        const auto next = [&]() -> std::optional<QString> {
            if (i + 1 >= args.size()) {
                return std::nullopt;
            }
            return args[++i];
        };
        std::optional<QString> value;
        if (arg == QLatin1String("--manifest") || arg == QLatin1String("--source-dir") ||
            arg == QLatin1String("--out-dir") || arg == QLatin1String("--scales") ||
            arg == QLatin1String("--depfile")) {
            value = next();
            if (!value) {
                return usage(arg.toStdString() + " needs a value");
            }
        } else {
            return usage("unexpected argument " + arg.toStdString());
        }
        if (arg == QLatin1String("--manifest")) {
            options.manifest = *value;
        } else if (arg == QLatin1String("--source-dir")) {
            options.sourceDir = *value;
        } else if (arg == QLatin1String("--out-dir")) {
            options.outDir = *value;
        } else if (arg == QLatin1String("--depfile")) {
            options.depfile = *value;
        } else {
            options.scales.clear();
            for (const QString& part : value->split(QLatin1Char(','))) {
                bool ok = false;
                const int scale = part.toInt(&ok);
                const bool known = std::ranges::find(frost::ui::kAtlasScales, scale) != frost::ui::kAtlasScales.end();
                if (!ok || !known || std::ranges::find(options.scales, scale) != options.scales.end()) {
                    return usage("--scales takes distinct values of 100,125,150,175,200,250,300");
                }
                options.scales.push_back(static_cast<std::uint16_t>(scale));
            }
        }
    }
    if (options.manifest.isEmpty() || options.sourceDir.isEmpty() || options.outDir.isEmpty()) {
        return usage("--manifest, --source-dir and --out-dir are required");
    }
    return bake(options);
}
