// SPDX-License-Identifier: Apache-2.0
// Bounded UTF-8 file admission and standalone diff/merge window ownership.
#include "cli_editor.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/utf.hpp"
#include "frost/ui/vcs/diff_viewer.hpp"
#include "frost/ui/vcs/merge_editor.hpp"

#include <QDialog>
#include <QFileInfo>
#include <QVBoxLayout>

#include <vector>
namespace frost::app {
Result<int> runCliEditor(const CommandLine& command) {
    auto& fs = io::realFileSystem();
    std::vector<std::string> paths, texts;
    const auto count = command.tool == CliTool::Diff ? 2u : 3u;
    for (std::size_t i = 0; i < command.paths.size(); ++i) {
        const auto bytes = QFileInfo(QString::fromUtf8(command.paths[i].c_str())).absoluteFilePath().toUtf8();
        paths.emplace_back(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        if (i >= count)
            continue;
        auto text = fs.readFileSnapshot(paths.back(), 16 * 1024 * 1024);
        if (!text)
            return std::unexpected(text.error());
        if (!io::isValidUtf8(*text) || text->find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "CLI editor requires UTF-8 text without NUL bytes");
        texts.push_back(std::move(*text));
    }
    if (command.tool == CliTool::Diff) {
        auto model = vcs::SideBySideDiff::compute(std::move(texts[0]), std::move(texts[1]));
        if (!model)
            return std::unexpected(model.error());
        QDialog window;
        window.setWindowTitle(QStringLiteral("FrostIDE — Diff"));
        window.resize(1200, 800);
        auto* layout = new QVBoxLayout(&window);
        auto* viewer = new ui::DiffViewer(&window);
        viewer->setDiff(std::move(*model), QString::fromUtf8(paths[0].c_str()), QString::fromUtf8(paths[1].c_str()));
        layout->addWidget(viewer);
        window.exec();
        return 0;
    }
    // Snapshot output too: applying must not silently overwrite edits made while this window was open.
    auto original = fs.readFileSnapshot(paths[3], 16 * 1024 * 1024);
    if (!original && original.error().code() != ErrorCode::NotFound)
        return std::unexpected(original.error());
    const bool existed = original.has_value();
    auto merged =
        ui::showMergeEditor({texts[0], texts[1], texts[2], paths[3]}, [&](std::string_view result) -> Result<void> {
            for (std::size_t i = 0; i < count; ++i) {
                auto current = fs.readFileSnapshot(paths[i], 16 * 1024 * 1024);
                if (!current || *current != texts[i])
                    return fail(ErrorCode::Busy, "merge source changed; reopen the merge");
            }
            auto current = fs.readFileSnapshot(paths[3], 16 * 1024 * 1024);
            if (existed ? (!current || *current != *original)
                        : (current || current.error().code() != ErrorCode::NotFound))
                return fail(ErrorCode::Busy, "merge output changed; reopen the merge");
            return fs.writeFileAtomic(paths[3], result);
        });
    if (!merged)
        return std::unexpected(merged.error());
    return *merged ? 0 : 1;
}
} // namespace frost::app
