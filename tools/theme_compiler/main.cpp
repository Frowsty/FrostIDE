// SPDX-License-Identifier: Apache-2.0
//
// frost-themec: compiles .theme.json files to theme blobs at build time (ADR 0015 decision 3).
//
/*
 * Usage: frost-themec [--lenient] --out-dir <dir> <file.theme.json>...
 *
 * Each input <stem>.theme.json is compiled as theme <stem> (which must be a valid theme id)
 * and written to <dir>/<stem>.ftheme, atomically (QSaveFile) and only when the bytes differ
 * from the existing file, so Ninja's restat prunes unchanged dependants (Budget: B28).
 * Diagnostics go to stderr as "<file>: <json path>: <severity>: <message>".
 * Exit status: 0 when no input has an Error diagnostic (warnings are printed); 1 when any input
 * has one or a write fails (nothing is written for that input); 2 on a usage error.
 */

#include "frost/ui/model/theme_blob.hpp"
#include "frost/ui/theme_compiler/theme_compiler.hpp"

#include "no_dialogs.hpp"
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <span>
#include <string>

namespace {

constexpr std::string_view kSuffix = ".theme.json";

void printLine(const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); }

int usage(const std::string& problem) {
    printLine("frost-themec: " + problem);
    printLine("usage: frost-themec [--lenient] --out-dir <dir> <file.theme.json>...");
    return 2;
}

// Writes `bytes` to `path` unless the file already holds exactly them.
bool writeIfChanged(const QString& path, const QByteArray& bytes, std::string& problem) {
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.size() == bytes.size() && existing.readAll() == bytes) {
        return true;
    }
    existing.close();
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        problem = out.errorString().toStdString();
        return false;
    }
    return true;
}

// Compiles one input; returns false on any Error diagnostic or failed write.
bool compileOne(const QString& input, const QString& outDir, const frost::ui::CompileOptions& options) {
    const std::string inputName = QDir::toNativeSeparators(input).toStdString();
    const std::string fileName = QFileInfo(input).fileName().toStdString();
    if (!fileName.ends_with(kSuffix)) {
        printLine(inputName + ": $: error: the file name must end in .theme.json");
        return false;
    }
    const std::string id = fileName.substr(0, fileName.size() - kSuffix.size());
    QFile file(input);
    if (!file.open(QIODevice::ReadOnly)) {
        printLine(inputName + ": $: error: cannot read: " + file.errorString().toStdString());
        return false;
    }
    // One byte over the limit is enough for the compiler to refuse the file.
    const QByteArray json = file.read(static_cast<qint64>(frost::ui::kMaxThemeJsonBytes) + 1);
    const frost::ui::CompileResult result = frost::ui::compileTheme(
        id, std::as_bytes(std::span(json.constData(), static_cast<std::size_t>(json.size()))), options);
    for (const frost::ui::Diagnostic& diagnostic : result.diagnostics) {
        printLine(inputName + ": " + frost::ui::formatDiagnostic(diagnostic));
    }
    if (!result.ok()) {
        return false;
    }
    const std::vector<std::byte> blob = frost::ui::encodeThemeBlob(*result.data);
    const QByteArray bytes(reinterpret_cast<const char*>(blob.data()), static_cast<qsizetype>(blob.size()));
    const QString outPath = QDir(outDir).filePath(QString::fromStdString(id + ".ftheme"));
    std::string problem;
    if (!writeIfChanged(outPath, bytes, problem)) {
        printLine(inputName + ": $: error: cannot write " + QDir::toNativeSeparators(outPath).toStdString() + ": " +
                  problem);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    frost::tools::disableToolDialogs();
    QCoreApplication app(argc, argv);
    // QCoreApplication::arguments() decodes the wide command line on Windows.
    const QStringList args = QCoreApplication::arguments();
    frost::ui::CompileOptions options;
    QString outDir;
    QStringList inputs;
    for (qsizetype i = 1; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (arg == QLatin1String("--lenient")) {
            options.strict = false;
        } else if (arg == QLatin1String("--out-dir")) {
            if (++i >= args.size()) {
                return usage("--out-dir needs a directory");
            }
            outDir = args[i];
        } else if (arg.startsWith(QLatin1String("--"))) {
            return usage("unknown option " + arg.toStdString());
        } else {
            inputs.push_back(arg);
        }
    }
    if (outDir.isEmpty()) {
        return usage("--out-dir is required");
    }
    if (inputs.isEmpty()) {
        return usage("no input files");
    }
    if (!QFileInfo(outDir).isDir()) {
        printLine("frost-themec: the output directory " + QDir::toNativeSeparators(outDir).toStdString() +
                  " does not exist");
        return 1;
    }
    bool ok = true;
    for (const QString& input : inputs) {
        ok = compileOne(input, outDir, options) && ok;
    }
    return ok ? 0 : 1;
}
