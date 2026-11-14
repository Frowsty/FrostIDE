// SPDX-License-Identifier: Apache-2.0
// FER-10 immutable test-source admission; workers never read a Workbench or another QObject.
#pragma once

#include "frost/project/trust_store.hpp"

#include "engine_unit_tests_window.hpp"

#include <atomic>
#include <memory>
#include <stop_token>
#include <string>

namespace frost::app {

struct EngineUnitTestsCapture {
    std::string lexicalRoot, physicalRoot, projectKey, project, program;
    std::uint64_t trustRevision = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> sourceRevision;
    std::shared_ptr<std::atomic<bool>> physicalAdmission;
    std::stop_token stop;
    std::optional<io::FileStatus> expectedProgramStatus;
    std::string physicalProgram{};
};

// GUI snapshot factory, O(path lengths), no filesystem calls. fs/trust outlive returned closures.
// Inputs are cached physical paths/status from the actual discovery/project owner. admitted is
// cheap logical trust/revision admission; validatePhysical is worker-only and performs metadata I/O.
// Errors identify missing cached identity, untrusted source or malformed captured selection.
[[nodiscard]] Result<EngineUnitTestsSource> captureEngineTestsSource(io::IFileSystem& fs, project::TrustStore& trust,
                                                                     EngineUnitTestsCapture capture);

} // namespace frost::app
