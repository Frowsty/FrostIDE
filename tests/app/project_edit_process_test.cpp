// SPDX-License-Identifier: Apache-2.0
// ProjectHost cancellation must reject output queued before the stop.
#include "frost/io/scripted_process_launcher.hpp"

#include "../../src/app/src/project_edit_process.hpp"
#include "workbench_rig.hpp"
#include <QThread>
#include <gtest/gtest.h>

TEST(ProjectEditProcess, StopRejectsLateReadyWithoutAdmitting) {
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = "{\"Ready\":true,\"TransactionId\":\"id\",\"JournalDigest\":\""
                                    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}\n",
                     .neverExits = true});
    frost::io::ProcessSpec spec;
#ifdef _WIN32
    spec.executable = "C:\\fake\\host.exe";
#else
    spec.executable = "/fake/host";
#endif
    bool admitted = false, completed = false;
    frost::app::ProjectEditProcess process(launcher, frost::systemClock());
    ASSERT_TRUE(process.start(
        spec, "{}", "id",
        [&](const std::string&) {
            admitted = true;
            return true;
        },
        [&](bool, bool, const std::string&) { completed = true; }));
    QThread::msleep(50); // Let the reader queue Ready without dispatching Qt events.
    process.stop();
    ASSERT_TRUE(frost::app::test::waitFor([&] { return completed; }));
    EXPECT_FALSE(admitted);
}
