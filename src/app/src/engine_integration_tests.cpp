// SPDX-License-Identifier: Apache-2.0
// FER-10 captures cached CLI/project identity on GUI; physical admission runs only on workers.
#include "frost/io/path.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/uri.hpp"

#include "engine_integration_internal.hpp"
#include "engine_unit_tests_source.hpp"

#include <utility>
#include <mutex>

namespace frost::app {

Result<EngineUnitTestsSource> captureEngineTestsSource(io::IFileSystem& fs, project::TrustStore& trust,
                                                       EngineUnitTestsCapture capture) {
    if (!capture.sourceRevision || !capture.physicalAdmission || capture.stop.stop_requested() ||
        !capture.physicalAdmission->load())
        return fail(ErrorCode::PermissionDenied, "The engine test project source is no longer current");
    if (!io::isAbsolutePath(capture.program) || !io::isAbsolutePath(capture.project) ||
        !io::isAbsolutePath(capture.physicalRoot) || lang::documentKey(capture.physicalRoot) != capture.projectKey)
        return fail(ErrorCode::PermissionDenied, "The cached engine test source identity is incomplete");
    const auto revision = capture.sourceRevision->load();
    EngineUnitTestsSource source;
    source.options.program = capture.program;
    source.options.expectedProgram = capture.physicalProgram;
    source.options.project = capture.project;
    source.options.expectedProjectRoot = capture.physicalRoot;
    if (lang::documentKey(capture.project) != lang::documentKey(capture.physicalRoot) &&
        lang::documentKey(capture.project) != lang::documentKey(capture.lexicalRoot))
        source.options.expectedProjectFile = capture.project;
    source.revision = revision;
    source.admitted = [&trust, capture, revision] {
        const auto trusted = trust.snapshot(capture.lexicalRoot); // In-memory mutex-protected snapshot, no I/O.
        return !capture.stop.stop_requested() && capture.physicalAdmission->load() &&
               capture.sourceRevision->load() == revision && trusted.trusted &&
               trusted.revision == capture.trustRevision;
    };
    struct SelectedIdentity {
        std::mutex mutex;
        std::optional<std::string> physical;
    };
    const auto selectedIdentity = std::make_shared<SelectedIdentity>();
    source.validatePhysical = [&fs, selectedIdentity, capture = std::move(capture), revision,
                               admitted = source.admitted]() -> Result<void> {
        if (!admitted())
            return fail(ErrorCode::PermissionDenied, "The engine test source or its trust changed");
        if (!capture.expectedProgramStatus || capture.physicalProgram.empty())
            return fail(ErrorCode::NotFound, "Refresh engine discovery to capture the actual CLI identity");
        if (!retainEngineProjectIdentity(fs, capture.lexicalRoot, capture.projectKey, capture.physicalAdmission))
            return fail(ErrorCode::PermissionDenied, "Reopen the project after its physical directory changes");
        const auto program = fs.physicalPath(capture.program);
        const auto status = program ? fs.status(*program) : Result<io::FileStatus>(fail(ErrorCode::NotFound));
        const auto selected = fs.physicalPath(capture.project);
        bool selectedCurrent = false;
        if (selected) {
            const auto selectedStatus = fs.status(*selected);
            if (selectedStatus) {
                const auto parent = selectedStatus->type == io::FileType::Directory
                                        ? *selected : std::string(io::parentPath(*selected));
                if (lang::documentKey(parent) == capture.projectKey &&
                    (selectedStatus->type == io::FileType::Directory ||
                     (selectedStatus->type == io::FileType::Regular && lang::isFrostEngineProjectFileName(*selected)))) {
                    std::lock_guard lock(selectedIdentity->mutex);
                    if (!selectedIdentity->physical)
                        selectedIdentity->physical = *selected;
                    selectedCurrent = lang::documentKey(*selectedIdentity->physical) == lang::documentKey(*selected);
                }
            }
        }
        if (!program || lang::documentKey(*program) != lang::documentKey(capture.physicalProgram) || !status ||
            *status != *capture.expectedProgramStatus || !selectedCurrent) {
            auto expected = revision;
            (void)capture.sourceRevision->compare_exchange_strong(expected, revision + 1);
            return fail(ErrorCode::PermissionDenied, "The physical engine CLI/project changed; refresh discovery");
        }
        if (!admitted())
            return fail(ErrorCode::PermissionDenied, "The engine test source changed during physical validation");
        return {};
    };
    if (!source.admitted())
        return fail(ErrorCode::Untrusted, "Trust the current engine test project before listing or running tests");
    return source;
}

Result<EngineUnitTestsSource> EngineIntegration::captureEngineUnitTestsSource() const {
    const auto& impl = *m_impl;
    if (!impl.available || !impl.admitted(impl.epoch, impl.root, impl.trustRevision))
        return fail(ErrorCode::Untrusted, "Open and trust a FrostEngine project to use its scene tests");
    if (impl.installations.empty() || impl.installations.front().cliProgram.empty() ||
        !impl.installations.front().cliStatus)
        return fail(ErrorCode::NotFound, "Locate a built FrostEditor CLI to list or run engine tests");
    const auto& tool = impl.installations.front();
    const auto project = lang::documentKey(impl.selectedProject) == impl.projectKey
                             ? impl.physicalRoot
                             : io::joinPath(impl.physicalRoot, io::fileName(impl.selectedProject)).value_or("");
    return captureEngineTestsSource(impl.workbench.m_services.fs, impl.workbench.m_services.trust,
                                    {impl.root, impl.physicalRoot, impl.projectKey, project, tool.cliProgram,
                                     impl.trustRevision, impl.testsSourceRevision, impl.physicalAdmission,
                                     impl.workStop.get_token(), tool.cliStatus, tool.physicalCliProgram});
}

} // namespace frost::app
