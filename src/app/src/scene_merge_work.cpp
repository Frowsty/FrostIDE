// SPDX-License-Identifier: Apache-2.0
// Loads scene conflict inputs and validates snapshots against the repository.
#include "scene_merge_work.hpp"

#include "frost/io/async_process.hpp"
#include "frost/io/path.hpp"
#include "frost/text/text_file.hpp"
#include "frost/vcs/conflict_sources.hpp"

#include <QTemporaryFile>

#include <algorithm>
#include <charconv>
namespace frost::app {
namespace {
Result<std::string> git(const SceneMergeWork& work, std::vector<std::string> args, std::stop_token stop) {
    if (!work.tool.admitted || !work.tool.admitted())
        return fail(ErrorCode::Untrusted, "The engine project source changed");
    vcs::GitInvocation call;
    call.arguments = std::move(args);
    call.workingDirectory = work.workTree;
    call.background = true;
    call.stop = stop;
    auto result = work.runner.run(call);
    if (!result)
        return std::unexpected(result.error());
    if (!result->succeeded())
        return std::unexpected(vcs::gitFailure("Read scene conflict sources", *result));
    return std::move(result->stdoutBytes);
}
Result<std::optional<std::string>> disk(io::IFileSystem& fs, const std::string& path) {
    auto value = fs.readFileSnapshot(path, 4 * 1024 * 1024);
    if (value)
        return std::optional<std::string>{std::move(*value)};
    if (value.error().code() == ErrorCode::NotFound)
        return std::optional<std::string>{};
    return std::unexpected(value.error());
}
class AdmittedLauncher final : public io::IProcessLauncher {
public:
    AdmittedLauncher(const SceneMergeWork& work, std::stop_token stop) : m_work(work), m_stop(stop) {}
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        if (auto valid = checkAdmission(); !valid)
            return std::unexpected(valid.error());
        return m_work.launcher.launch(spec);
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                         const io::AsyncContext& context) override {
        if (auto valid = checkAdmission(); !valid)
            return std::unexpected(valid.error());
        return m_work.launcher.launchAsync(spec, context);
    }

private:
    Result<void> checkAdmission() const {
        if (m_stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Scene merge cancelled");
        if (!m_work.tool.admitted || !m_work.tool.admitted())
            return fail(ErrorCode::Untrusted, "Engine merge source admission changed before launch");
        return {};
    }

    const SceneMergeWork& m_work;
    std::stop_token m_stop;
};
} // namespace
Result<void> validateSceneMergeSources(const SceneMergeWork& work, const SceneMergeSnapshot& source,
                                       std::stop_token stop) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Scene merge cancelled");
    if (!work.tool.admitted || !work.tool.admitted())
        return fail(ErrorCode::Untrusted, "Engine merge project or trust changed");
    if (!work.tool.validatePhysical)
        return fail(ErrorCode::Unsupported, "Engine tool has no physical source admission");
    if (auto valid = work.tool.validatePhysical(); !valid)
        return valid;
    auto target = work.fs.physicalPath(work.absolute);
    if (!target || *target != source.target)
        return fail(ErrorCode::Busy, "Scene merge output identity changed");
    auto stages = git(work, {"ls-files", "--unmerged", "-z", "--", work.relative}, stop);
    auto indexPath = io::joinPath(work.gitDir, "index");
    if (!indexPath)
        return std::unexpected(indexPath.error());
    auto index = work.fs.readFileSnapshot(*indexPath, 64 * 1024 * 1024);
    auto bytes = disk(work.fs, work.absolute);
    if (!stages || !index || !bytes)
        return fail(ErrorCode::IoError, "Could not recheck scene merge source receipts");
    if (*stages != source.stages || *index != source.index || *bytes != source.disk)
        return fail(ErrorCode::Busy, "Git stages, index or working file changed; reopen the semantic merge");
    return {};
}
Result<SceneMergeSnapshot> refreshSceneMerge(const SceneMergeWork& work, SceneMergeSnapshot source,
                                             std::stop_token stop) {
    if (auto valid = validateSceneMergeSources(work, source, stop); !valid)
        return std::unexpected(valid.error());
    auto params = engine::sceneMergeParams(source.request);
    if (!params)
        return std::unexpected(params.error());
    // A private file avoids Windows argv limits. It remains owned until child completion.
    QTemporaryFile file;
    if (!file.open())
        return fail(ErrorCode::IoError, "Could not create private scene merge request");
    const auto bytes = params->dump();
    if (file.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()) ||
        !file.flush())
        return fail(ErrorCode::IoError, "Could not write private scene merge request");
    const auto filename = file.fileName().toStdString();
    file.close();
    engine::EngineCliRequest request;
    request.command = "document.merge-scene";
    request.paramsFile = filename;
    request.project = work.tool.options.project;
    request.workingDirectory = work.tool.options.expectedProjectRoot;
    request.stopToken = stop;
    request.timeout = std::chrono::seconds(60);
    AdmittedLauncher launcher(work, stop);
    auto actual = engine::runEngineCliResult(launcher, work.tool.options.program, request);
    if (!actual)
        return std::unexpected(actual.error());
    auto report = engine::parseSceneMergeReport(*actual, source.request);
    if (!report)
        return std::unexpected(report.error());
    if (auto valid = validateSceneMergeSources(work, source, stop); !valid)
        return std::unexpected(valid.error());
    source.report = std::move(*report);
    return source;
}
Result<SceneMergeSnapshot> prepareSceneMerge(const SceneMergeWork& work, std::stop_token stop) {
    SceneMergeSnapshot source;
    auto target = work.fs.physicalPath(work.absolute);
    if (!target)
        return std::unexpected(target.error());
    source.target = *target;
    auto root = work.fs.physicalPath(work.tool.options.expectedProjectRoot);
    if (!root)
        return std::unexpected(root.error());
    // Only the selected project's actual physical Assets subtree can supply engine scene semantics.
    const auto rootPath = io::toStdPath(*root);
    if (!rootPath)
        return std::unexpected(rootPath.error());
    const auto targetPath = io::toStdPath(*target);
    if (!targetPath)
        return std::unexpected(targetPath.error());
    auto relativePath = io::fromStdPath(targetPath->lexically_relative(*rootPath));
    if (!relativePath)
        return std::unexpected(relativePath.error());
    source.request.images.path = std::move(*relativePath);
    std::ranges::replace(source.request.images.path, '\\', '/');
    auto stages = git(work, {"ls-files", "--unmerged", "-z", "--", work.relative}, stop);
    if (!stages)
        return std::unexpected(stages.error());
    source.stages = *stages;
    auto parsed = vcs::parseConflictStages(source.stages, work.relative);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (!(*parsed)[1] || !(*parsed)[2])
        return fail(
            ErrorCode::Unsupported,
            "Semantic merge requires both actual ours and theirs stages; use normal conflict choices for deletions");
    auto indexPath = io::joinPath(work.gitDir, "index");
    if (!indexPath)
        return std::unexpected(indexPath.error());
    auto index = work.fs.readFileSnapshot(*indexPath, 64 * 1024 * 1024);
    auto bytes = disk(work.fs, work.absolute);
    if (!index || !bytes)
        return fail(ErrorCode::IoError, "Could not capture scene merge source receipts");
    source.index = std::move(*index);
    source.disk = std::move(*bytes);
    source.request.images.baseExists = (*parsed)[0].has_value();
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(*parsed)[i])
            continue;
        auto size = git(work, {"cat-file", "-s", (*parsed)[i]->object}, stop);
        if (!size)
            return std::unexpected(size.error());
        const auto trimmed = vcs::trimGitText(*size);
        std::uint64_t count = 0;
        const auto parsedSize = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), count);
        if (parsedSize.ec != std::errc{} || parsedSize.ptr != trimmed.data() + trimmed.size() || count > 1024 * 1024)
            return fail(ErrorCode::OutOfRange, "Scene merge Git blob exceeds the bounded input profile");
        auto blob = git(work, {"cat-file", "blob", (*parsed)[i]->object}, stop);
        if (!blob)
            return std::unexpected(blob.error());
        if (blob->size() > 1024 * 1024)
            return fail(ErrorCode::OutOfRange, "Scene merge input exceeds 1 MiB");
        text::LoadOptions options;
        options.encoding = text::Encoding::utf8();
        auto decoded = text::decodeTextFile(std::move(*blob), options);
        if (!decoded || decoded->binaryLikely || !decoded->encoding.fidelity.lossless() ||
            decoded->encoding.format.encoding != text::Encoding::utf8())
            return fail(ErrorCode::Corrupt, "Semantic scene merge requires losslessly decoded UTF8 Git blobs");
        auto image = decoded->buffer.snapshot().text();
        if (i == 0)
            source.request.images.base = std::move(image);
        else if (i == 1)
            source.request.images.ours = std::move(image);
        else
            source.request.images.theirs = std::move(image);
    }
    return refreshSceneMerge(work, std::move(source), stop);
}
} // namespace frost::app
