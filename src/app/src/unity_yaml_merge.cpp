// SPDX-License-Identifier: Apache-2.0
#include "unity_yaml_merge.hpp"

#include "frost/io/async_process.hpp"
#include "frost/io/path.hpp"
#include "frost/text/text_file.hpp"
#include "frost/vcs/conflict_sources.hpp"

#include <QTemporaryDir>

#include <charconv>
namespace frost::app {
namespace {
constexpr std::size_t limit = 8 * 1024 * 1024;
Result<void> admitted(const UnityMergeWork& work, std::stop_token stop) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "UnityYAMLMerge cancelled");
    if (!work.admitted || !work.admitted() || !work.git.trusted())
        return fail(ErrorCode::Untrusted, "Unity merge project or trust admission changed");
    return {};
}
Result<std::string> git(const UnityMergeWork& work, std::vector<std::string> args, std::stop_token stop) {
    if (auto valid = admitted(work, stop); !valid)
        return std::unexpected(valid.error());
    vcs::GitInvocation call;
    call.arguments = std::move(args);
    call.workingDirectory = work.workTree;
    call.background = true;
    call.stop = stop;
    auto output = work.git.run(call);
    if (!output)
        return std::unexpected(output.error());
    if (!output->succeeded())
        return std::unexpected(vcs::gitFailure("Read Unity merge stages", *output));
    return std::move(output->stdoutBytes);
}
Result<std::string> utf8(std::string bytes) {
    text::LoadOptions options;
    options.encoding = text::Encoding::utf8();
    auto decoded = text::decodeTextFile(std::move(bytes), options);
    if (!decoded || decoded->binaryLikely || !decoded->encoding.fidelity.lossless() ||
        decoded->encoding.format.encoding != text::Encoding::utf8())
        return fail(ErrorCode::Corrupt, "Unity merge requires losslessly decoded UTF8 files");
    return decoded->buffer.snapshot().text();
}
bool markers(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        auto end = text.find('\n', at);
        auto line = text.substr(at, end == text.npos ? end : end - at);
        if (line.starts_with("<<<<<<<") || line.starts_with("=======") || line.starts_with(">>>>>>>") ||
            line.starts_with("|||||||"))
            return true;
        if (end == text.npos)
            break;
        at = end + 1;
    }
    return false;
}
class AdmittedLauncher final : public io::IProcessLauncher {
public:
    AdmittedLauncher(const UnityMergeWork& work, const UnityMergePreview& source, std::stop_token stop)
        : m_work(work), m_source(source), m_stop(stop) {}
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        if (auto valid = validateUnityMergeSources(m_work, m_source, m_stop); !valid)
            return std::unexpected(valid.error());
        return m_work.launcher.launch(spec);
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                         const io::AsyncContext& context) override {
        if (auto valid = validateUnityMergeSources(m_work, m_source, m_stop); !valid)
            return std::unexpected(valid.error());
        return m_work.launcher.launchAsync(spec, context);
    }

private:
    const UnityMergeWork& m_work;
    const UnityMergePreview& m_source;
    std::stop_token m_stop;
};
} // namespace
Result<void> validateUnityMergeSources(const UnityMergeWork& work, const UnityMergePreview& source,
                                       std::stop_token stop) {
    if (auto valid = admitted(work, stop); !valid)
        return valid;
    auto root = work.fs.physicalPath(work.root), target = work.fs.physicalPath(work.absolute),
         program = work.fs.physicalPath(work.program);
    auto status = work.fs.status(work.program);
    if (!root || !target || !program || !status || *root != source.physicalRoot || *target != source.physicalTarget ||
        *program != source.physicalProgram || *status != source.programStatus)
        return fail(ErrorCode::Busy, "Unity merge project, target or selected tool identity changed");
    auto stages = git(work, {"ls-files", "--unmerged", "-z", "--", work.relative}, stop);
    auto indexPath = io::joinPath(work.gitDir, "index");
    if (!indexPath)
        return std::unexpected(indexPath.error());
    auto index = work.fs.readFileSnapshot(*indexPath, 64 * 1024 * 1024),
         disk = work.fs.readFileSnapshot(work.absolute, limit);
    if (!stages || !index || !disk)
        return fail(ErrorCode::IoError, "Could not recheck Unity merge source receipts");
    if (*stages != source.stages || *index != source.index || *disk != source.disk)
        return fail(ErrorCode::Busy, "Git index, stages or working file changed; reopen the Unity merge");
    return {};
}
Result<UnityMergePreview> prepareUnityMerge(const UnityMergeWork& work, std::stop_token stop) {
    if (auto valid = admitted(work, stop); !valid)
        return std::unexpected(valid.error());
    const auto name = io::fileName(work.program);
#ifdef _WIN32
    if (name != "UnityYAMLMerge.exe")
#else
    if (name != "UnityYAMLMerge")
#endif
        return fail(ErrorCode::InvalidArgument, "Select an installed UnityYAMLMerge executable");
    if (!io::isAbsolutePath(work.program) || (!work.relative.ends_with(".unity") &&
                                              !work.relative.ends_with(".prefab") && !work.relative.ends_with(".meta")))
        return fail(ErrorCode::Unsupported, "UnityYAMLMerge supports selected scene, prefab or meta conflicts");
    UnityMergePreview source;
    auto root = work.fs.physicalPath(work.root), target = work.fs.physicalPath(work.absolute),
         program = work.fs.physicalPath(work.program);
    auto status = work.fs.status(work.program);
    if (!root || !target || !program || !status || status->type != io::FileType::Regular)
        return fail(ErrorCode::NotFound, "The selected Unity merge tool/project/working file is unavailable");
    source.physicalRoot = *root;
    source.physicalTarget = *target;
    source.physicalProgram = *program;
    source.programStatus = *status;
    auto versionPath = io::joinPath(work.root, "ProjectSettings/ProjectVersion.txt");
    if (!versionPath)
        return std::unexpected(versionPath.error());
    auto version = work.fs.readFileSnapshot(*versionPath, 1024 * 1024);
    if (!version)
        return fail(ErrorCode::InvalidArgument, "Open the actual Unity project root before using UnityYAMLMerge");
    auto stages = git(work, {"ls-files", "--unmerged", "-z", "--", work.relative}, stop);
    if (!stages)
        return std::unexpected(stages.error());
    source.stages = *stages;
    auto parsed = vcs::parseConflictStages(source.stages, work.relative);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (!(*parsed)[0] || !(*parsed)[1] || !(*parsed)[2])
        return fail(ErrorCode::Unsupported, "UnityYAMLMerge requires actual base, ours and theirs stages; use normal "
                                            "conflict choices for add/delete conflicts");
    auto indexPath = io::joinPath(work.gitDir, "index");
    if (!indexPath)
        return std::unexpected(indexPath.error());
    auto index = work.fs.readFileSnapshot(*indexPath, 64 * 1024 * 1024),
         disk = work.fs.readFileSnapshot(work.absolute, limit);
    if (!index || !disk)
        return fail(ErrorCode::IoError, "Could not capture Unity merge index/working file");
    source.index = *index;
    source.disk = *disk;
    if (auto decoded = utf8(source.disk); !decoded)
        return std::unexpected(decoded.error());
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return fail(ErrorCode::IoError, "Could not create private Unity merge directory");
    std::array<std::string, 3> paths;
    const auto filename = std::string(io::fileName(work.relative));
    for (std::size_t i = 0; i < 3; ++i) {
        auto size = git(work, {"cat-file", "-s", (*parsed)[i]->object}, stop);
        if (!size)
            return std::unexpected(size.error());
        const auto trimmed = vcs::trimGitText(*size);
        std::uint64_t bytes = 0;
        auto count = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), bytes);
        if (count.ec != std::errc{} || count.ptr != trimmed.data() + trimmed.size() || bytes > limit)
            return fail(ErrorCode::OutOfRange, "Unity merge Git image exceeds 8 MiB");
        auto blob = git(work, {"cat-file", "blob", (*parsed)[i]->object}, stop);
        if (!blob)
            return std::unexpected(blob.error());
        if (blob->size() > limit)
            return fail(ErrorCode::OutOfRange, "Unity merge Git image exceeds 8 MiB");
        auto image = utf8(*blob);
        if (!image)
            return std::unexpected(image.error());
        if (i == 0)
            source.base = *image;
        else if (i == 1)
            source.ours = *image;
        else
            source.theirs = *image;
        paths[i] = temporary.filePath(QString::fromStdString(std::to_string(i) + "-" + filename)).toStdString();
        if (auto written = work.fs.writeFileAtomic(paths[i], *blob); !written)
            return std::unexpected(written.error());
    }
    const auto output = temporary.filePath(QString::fromStdString("merged-" + filename)).toStdString();
    const auto conflicts = temporary.filePath(QStringLiteral("conflicts.txt")).toStdString();
    // Official Unity CLI order is base,theirs,mine,output. Headless and no fallback keep all
    // vendor writes in these copies and never launch an unrelated interactive merge program.
    // https://docs.unity.com/en-us/engine/6000.3/manual/get-started/project-configuration/version-control/smart-merge
    io::ProcessSpec spec;
    spec.executable = work.program;
    spec.arguments = {"merge", "-h", "--fallback", "none", "-o", conflicts, paths[0], paths[2], paths[1], output};
    spec.workingDirectory = temporary.path().toStdString();
    AdmittedLauncher launcher(work, source, stop);
    auto result = io::runToCompletionUntil(launcher, spec, {}, std::chrono::seconds(60), stop);
    if (!result)
        return std::unexpected(result.error());
    if (auto valid = validateUnityMergeSources(work, source, stop); !valid)
        return std::unexpected(valid.error());
    source.exitCode = result->exitCode;
    source.diagnostics =
        "Exit code: " + std::to_string(source.exitCode) + "\n" + result->stdoutBytes + "\n" + result->stderrBytes;
    auto report = work.fs.readFileSnapshot(conflicts, limit);
    if (report)
        source.diagnostics += "\n" + *report;
    else if (report.error().code() != ErrorCode::NotFound)
        return std::unexpected(report.error());
    auto merged = work.fs.readFileSnapshot(output, limit);
    if (merged) {
        auto decoded = utf8(std::move(*merged));
        if (!decoded)
            return std::unexpected(decoded.error());
        source.output = std::move(*decoded);
    } else if (merged.error().code() != ErrorCode::NotFound)
        return std::unexpected(merged.error());
    source.clean =
        source.exitCode == 0 && merged.has_value() && (!report || report->empty()) && !markers(source.output);
    return source;
}
} // namespace frost::app
