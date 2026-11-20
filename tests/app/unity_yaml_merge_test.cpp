// SPDX-License-Identifier: Apache-2.0
// Genuine disposable Git stages; a deliberately scripted vendor process tests isolation/admission.
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include "unity_yaml_merge.hpp"
#include "vcs/git_fixture.hpp"
#include <QApplication>
#include <QDialog>
#include <QPushButton>
#include <QTimer>
#include <gtest/gtest.h>

#include <algorithm>
namespace frost::app {
namespace {
class VendorFixture final : public io::IProcessLauncher {
public:
    io::IFileSystem& fs;
    std::string base, theirs, ours, output, report;
    std::function<void()> during;
    int code = 0;
    io::ScriptedProcessLauncher scripted;
    std::vector<std::string> arguments;
    explicit VendorFixture(io::IFileSystem& value) : fs(value) {}
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        arguments = spec.arguments;
        EXPECT_EQ(arguments.size(), 10U);
        if (arguments.size() != 10)
            return fail(ErrorCode::Corrupt, "unexpected vendor arguments");
        EXPECT_EQ(arguments[0], "merge");
        EXPECT_EQ(arguments[1], "-h");
        EXPECT_EQ(arguments[2], "--fallback");
        EXPECT_EQ(arguments[3], "none");
        EXPECT_EQ(arguments[4], "-o");
        EXPECT_EQ(fs.readFile(arguments[6]).value_or("missing"), base);
        EXPECT_EQ(fs.readFile(arguments[7]).value_or("missing"), theirs);
        EXPECT_EQ(fs.readFile(arguments[8]).value_or("missing"), ours);
        EXPECT_EQ(io::parentPath(arguments[6]), spec.workingDirectory);
        EXPECT_TRUE(fs.writeFileAtomic(arguments[9], output));
        if (!report.empty())
            EXPECT_TRUE(fs.writeFileAtomic(arguments[5], report));
        if (during)
            during();
        scripted.expect({.exitCode = code, .stdoutBytes = "vendor stdout", .stderrBytes = "vendor stderr"});
        return scripted.launch(spec);
    }
};
TEST(UnityYamlMerge, UsesIsolatedActualGitStagesAndReportsVendorConflictsWithoutApplying) {
    vcs::test::TempRepo repo("unity-yaml-merge");
    repo.init();
    const std::string path = "Assets/Main.unity";
    const std::string base = "%YAML 1.1\n--- !u!1 &1\nGameObject:\n  m_Name: Base\n",
                      ours = "%YAML 1.1\n--- !u!1 &1\nGameObject:\n  m_Name: Ours\n",
                      theirs = "%YAML 1.1\n--- !u!1 &1\nGameObject:\n  m_Name: Theirs\n";
    repo.write("ProjectSettings/ProjectVersion.txt", "m_EditorVersion: fixture\n");
    repo.write(path, base);
    repo.commitAll("base");
    repo.git({"checkout", "-b", "topic"});
    repo.write(path, theirs);
    repo.commitAll("theirs");
    repo.git({"checkout", "main"});
    repo.write(path, ours);
    repo.commitAll("ours");
    repo.git({"merge", "topic"}, 1);
#ifdef _WIN32
    const auto tool = repo.path("installed/UnityYAMLMerge.exe");
#else
    const auto tool = repo.path("installed/UnityYAMLMerge");
#endif
    ASSERT_TRUE(repo.fs().createDirectories(io::parentPath(tool)));
    ASSERT_TRUE(repo.fs().writeFileAtomic(tool, "scripted test tool, never executed"));
    VendorFixture vendor(repo.fs());
    vendor.base = base;
    vendor.ours = ours;
    vendor.theirs = theirs;
    vendor.output = ours;
    bool trusted = true;
    UnityMergeWork work{repo.fs(),          vendor,          repo.runner(), repo.work(), repo.work(),
                        repo.info().gitDir, repo.path(path), path,          tool,        [&] {return trusted; }};
    const auto original = repo.read(path);
    auto clean = prepareUnityMerge(work, {});
    ASSERT_TRUE(clean) << clean.error().describe();
    EXPECT_TRUE(clean->clean);
    EXPECT_EQ(clean->output, ours);
    EXPECT_EQ(repo.read(path), original);
    const auto status = repo.status();
    EXPECT_TRUE(std::ranges::any_of(status.entries, [](const vcs::GitStatusEntry& entry) {
        return entry.fileStatus() == vcs::VcsFileStatus::Conflicted;
    }));
    ASSERT_EQ(vendor.arguments.size(), 10U);
    EXPECT_FALSE(repo.fs().exists(vendor.arguments[6]).value_or(true));
    vendor.report = "Unresolved serialized object 1 field m_Name";
    auto unresolved = prepareUnityMerge(work, {});
    ASSERT_TRUE(unresolved);
    EXPECT_FALSE(unresolved->clean);
    EXPECT_NE(unresolved->diagnostics.find(vendor.report), std::string::npos);
    EXPECT_EQ(repo.read(path), original);
    vendor.report.clear();
    vendor.code = 1;
    auto failed = prepareUnityMerge(work, {});
    ASSERT_TRUE(failed);
    EXPECT_FALSE(failed->clean);
    EXPECT_EQ(failed->exitCode, 1);
    vendor.code = 0;
    vendor.during = [&] { repo.write(path, "foreign edit"); };
    EXPECT_FALSE(prepareUnityMerge(work, {}));
    repo.write(path, original);
    vendor.during = [&] { trusted = false; };
    EXPECT_FALSE(prepareUnityMerge(work, {}));
    trusted = true;
    vendor.during = {};
    repo.git({"add", "--", path});
    EXPECT_FALSE(validateUnityMergeSources(work, *clean, {}));
}
TEST(UnityYamlMerge, UnresolvedPreviewCannotApplyOrStage) {
    UnityMergePreview preview;
    preview.ours = "ours";
    preview.output = "vendor output";
    preview.diagnostics = "Actual unresolved conflict";
    preview.clean = false;
    QWidget owner;
    int applied = 0;
    bool found = false;
    QTimer drive;
    drive.setInterval(1);
    QObject::connect(&drive, &QTimer::timeout, &owner, [&] {
        for (auto* w : QApplication::topLevelWidgets())
            if (auto* d = qobject_cast<QDialog*>(w);
                d && d->objectName() == QStringLiteral("unityMergePreviewDialog")) {
                auto* button = d->findChild<QPushButton*>(QStringLiteral("unityMergeApply"));
                ASSERT_NE(button, nullptr);
                EXPECT_FALSE(button->isEnabled());
                found = true;
                d->reject();
            }
    });
    drive.start();
    auto result = showUnityMergePreview(&owner, preview, [&](std::string_view) -> Result<void> {
        ++applied;
        return {};
    });
    ASSERT_TRUE(result);
    EXPECT_FALSE(*result);
    EXPECT_TRUE(found);
    EXPECT_EQ(applied, 0);
}
} // namespace
} // namespace frost::app
