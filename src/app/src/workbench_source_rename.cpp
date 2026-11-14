// SPDX-License-Identifier: Apache-2.0
// File and project membership edits are prepared together, before any disk or editor effects.
#include "workbench_source_rename.hpp"

#include "frost/lang/uri.hpp"

#include <algorithm>
#include <cstdint>

namespace frost::app {
namespace {
constexpr std::uint64_t kMaxInputBytes = 64 * 1024 * 1024;
bool samePath(std::string_view a, std::string_view b) { return lang::documentKey(a) == lang::documentKey(b); }
Result<std::pair<std::string, text::Encoding>> decodedProjectInput(std::string_view bytes) {
    text::LoadOptions options;
    options.maxFileBytes = kMaxInputBytes;
    options.buffer.maxBufferBytes = kMaxInputBytes;
    auto decoded = text::decodeBytes(bytes, options);
    if (!decoded)
        return std::unexpected(decoded.error());
    if (decoded->encoding.fidelity.loss != text::LossKind::None || decoded->binaryLikely)
        return fail(ErrorCode::Unsupported, "Project input cannot be decoded losslessly");
    return std::pair{decoded->payloadIsText ? std::string(bytes.substr(decoded->payloadOffset))
                                            : std::move(decoded->text),
                     decoded->encoding.format.encoding};
}
} // namespace

Result<RefactoringInputSnapshot> captureRefactoringInput(io::IFileSystem& fs, std::string path,
                                                         std::optional<std::string> openText) {
    RefactoringInputSnapshot captured{std::move(path), {}, std::move(openText)};
    if (captured.buffer && captured.buffer->size() > kMaxInputBytes)
        return fail(ErrorCode::Unsupported, "Project buffer exceeds the refactoring input limit");
    auto status = fs.status(captured.path);
    if (!status) {
        if (status.error().code() == ErrorCode::NotFound)
            return captured;
        return std::unexpected(status.error());
    }
    if (status->type != io::FileType::Regular || status->size > kMaxInputBytes)
        return fail(ErrorCode::Unsupported, "Refactoring input must be a bounded regular file: " + captured.path);
    auto bytes = fs.readFile(captured.path);
    if (!bytes)
        return std::unexpected(bytes.error());
    if (bytes->size() > kMaxInputBytes)
        return fail(ErrorCode::Unsupported, "Refactoring input changed beyond its size limit: " + captured.path);
    captured.disk = std::move(*bytes);
    return captured;
}

Result<void> appendRefactoringDependency(document::RefactoringPlan& plan, const RefactoringInputSnapshot& input) {
    auto disk = std::ranges::find_if(plan.dependencies,
                                     [&](const auto& existing) { return samePath(existing.path, input.path); });
    if (disk != plan.dependencies.end()) {
        if (disk->expectedBytes != input.disk)
            return fail(ErrorCode::Busy, "Input changed while preparing rename: " + input.path);
    } else {
        plan.dependencies.push_back({input.path, input.disk});
    }
    if (input.buffer) {
        auto buffer = std::ranges::find_if(plan.bufferDependencies,
                                           [&](const auto& existing) { return samePath(existing.path, input.path); });
        if (buffer != plan.bufferDependencies.end()) {
            if (buffer->expectedText != *input.buffer)
                return fail(ErrorCode::Busy, "Open input changed while preparing rename: " + input.path);
        } else {
            plan.bufferDependencies.push_back({input.path, *input.buffer});
        }
    }
    return {};
}

Result<void> appendRefactoringReplacement(document::RefactoringPlan& plan, const RefactoringInputSnapshot& input,
                                          std::string_view before, std::string after) {
    std::optional<std::string> afterText;
    if (input.buffer) {
        auto decodedBefore = decodedProjectInput(before);
        auto decodedAfter = decodedProjectInput(after);
        if (!decodedBefore)
            return std::unexpected(decodedBefore.error());
        if (!decodedAfter)
            return std::unexpected(decodedAfter.error());
        if (decodedBefore->first != *input.buffer)
            return fail(ErrorCode::Busy, "Membership proposal does not match the open input: " + input.path);
        if (decodedBefore->second != decodedAfter->second)
            return fail(ErrorCode::Unsupported,
                        "Close the project definition before applying an encoding-changing serialization");
        afterText = std::move(decodedAfter->first);
    } else if (!input.disk || *input.disk != before) {
        return fail(ErrorCode::Busy, "Membership proposal does not match its captured input: " + input.path);
    }
    if (std::ranges::any_of(plan.renames, [&](const auto& rename) {
            return samePath(rename.from, input.path) || samePath(rename.to, input.path);
        }))
        return fail(ErrorCode::Unsupported, "A renamed source cannot also be a membership replacement");
    if (auto dependency = appendRefactoringDependency(plan, input); !dependency)
        return dependency;
    if (before == after)
        return {};
    if (std::ranges::any_of(plan.disk, [&](const auto& edit) { return samePath(edit.path, input.path); }) ||
        std::ranges::any_of(plan.buffers, [&](const auto& edit) { return samePath(edit.path, input.path); }))
        return fail(ErrorCode::Unsupported, "Membership inputs require one merged replacement: " + input.path);
    if (input.buffer) {
        plan.buffers.push_back(
            {input.path, *input.buffer, std::move(*afterText), document::BufferPersistence::Automatic});
    } else {
        if (!input.disk)
            return fail(ErrorCode::NotFound, "Membership definition is missing: " + input.path);
        plan.disk.push_back({input.path, *input.disk, std::move(after)});
    }
    return {};
}

Result<void> refactoringInputMatchesEvaluation(const RefactoringInputSnapshot& input,
                                               const std::optional<std::string>& evaluatedBytes) {
    if (input.disk != evaluatedBytes)
        return fail(ErrorCode::Busy, "Project input changed since helper evaluation: " + input.path);
    if (input.buffer) {
        if (!evaluatedBytes)
            return fail(ErrorCode::Busy, "Unsaved project input was not observed by ProjectHost: " + input.path);
        auto decoded = decodedProjectInput(*evaluatedBytes);
        if (!decoded)
            return std::unexpected(decoded.error());
        if (decoded->first != *input.buffer)
            return fail(ErrorCode::Busy,
                        "Save the changed project/import buffer before preparing rename: " + input.path);
    }
    return {};
}

Result<document::RefactoringPlan> prepareSourceFileRename(io::IFileSystem& fs, const lang::FmetaMovePlan& move,
                                                          std::string label) {
    document::RefactoringPlan plan;
    plan.label = std::move(label);
    auto source = captureRefactoringInput(fs, move.sourcePath);
    auto target = captureRefactoringInput(fs, move.targetPath);
    if (!source)
        return std::unexpected(source.error());
    if (!target)
        return std::unexpected(target.error());
    if (!source->disk)
        return fail(ErrorCode::NotFound, "Rename source is missing");
    if (target->disk)
        return fail(ErrorCode::AlreadyExists, "Rename destination already exists");
    if (lang::isFmetaPath(move.sourcePath) || lang::isFmetaPath(move.targetPath))
        return fail(ErrorCode::InvalidArgument, "Rename the asset, not its .fmeta sidecar");
    plan.renames.push_back({move.sourcePath, move.targetPath, *source->disk});
    if (auto added = appendRefactoringDependency(plan, *source); !added)
        return std::unexpected(added.error());
    if (auto added = appendRefactoringDependency(plan, *target); !added)
        return std::unexpected(added.error());
    auto sidecarSourcePath = lang::fmetaSidecarPath(move.sourcePath);
    auto sidecarTargetPath = lang::fmetaSidecarPath(move.targetPath);
    if (!sidecarSourcePath)
        return std::unexpected(sidecarSourcePath.error());
    if (!sidecarTargetPath)
        return std::unexpected(sidecarTargetPath.error());
    auto sidecarSource = captureRefactoringInput(fs, *sidecarSourcePath);
    auto sidecarTarget = captureRefactoringInput(fs, *sidecarTargetPath);
    if (!sidecarSource)
        return std::unexpected(sidecarSource.error());
    if (!sidecarTarget)
        return std::unexpected(sidecarTarget.error());
    if (sidecarTarget->disk)
        return fail(ErrorCode::AlreadyExists, "Destination .fmeta already exists");
    if (move.hasSidecar() != sidecarSource->disk.has_value() ||
        (move.hasSidecar() && (!move.targetSidecarPath || !samePath(*move.sourceSidecarPath, *sidecarSourcePath) ||
                               !samePath(*move.targetSidecarPath, *sidecarTargetPath))))
        return fail(ErrorCode::Busy, "Sidecar changed while preparing rename");
    if (sidecarSource->disk)
        plan.renames.push_back({*sidecarSourcePath, *sidecarTargetPath, *sidecarSource->disk});
    if (auto added = appendRefactoringDependency(plan, *sidecarSource); !added)
        return std::unexpected(added.error());
    if (auto added = appendRefactoringDependency(plan, *sidecarTarget); !added)
        return std::unexpected(added.error());
    return plan;
}

} // namespace frost::app
