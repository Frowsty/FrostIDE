// SPDX-License-Identifier: Apache-2.0
// Checks that untrusted managed tools cannot launch or publish test discovery rows.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "../../src/app/src/managed_tools_window.hpp"
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QWidget>
#include <gtest/gtest.h>

namespace frost::app {
TEST(ManagedToolsWindow, UntrustedNormalActionsNeverLaunchAndDiscoveryHasNoInventedRows) {
    const auto root =
        io::kNativePathStyle == io::PathStyle::Windows ? std::string("C:/managed-test") : std::string("/managed-test");
    io::MemFs fs{systemClock()};
    io::ScriptedProcessLauncher launcher;
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.writeFileAtomic(root + "/Tests.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\"/>"));
    auto trust = project::TrustStore::open(fs, root + "/trust.txt");
    ASSERT_TRUE(trust);
    ManagedToolsContext context;
    context.fs = &fs;
    context.launcher = &launcher;
    context.trust = trust->get();
    context.root = [root] { return root; };
    context.dotnet = [root] { return root + "/dotnet"; };
    context.projects = [root] { return std::vector<std::string>{root + "/Tests.csproj"}; };
    std::unique_ptr<QWidget> widget(createDotnetTestsWidget(nullptr, context));
    auto* framework = widget->findChild<QLineEdit*>("managedTargetFramework");
    ASSERT_NE(framework, nullptr);
    framework->setText("net10.0");
    for (auto* button : widget->findChildren<QPushButton*>())
        if (button->text() == "Discover" || button->text() == "Run All")
            button->click();
    EXPECT_TRUE(launcher.launches().empty());
    ASSERT_NE(widget->findChild<QTableWidget*>(), nullptr);
    EXPECT_EQ(widget->findChild<QTableWidget*>()->rowCount(), 0);
    std::unique_ptr<QWidget> packages(createManagedPackagesWidget(nullptr, context));
    QPushButton* installButton = nullptr;
    for (auto* button : packages->findChildren<QPushButton*>()) {
        if (button->text() == "Install…")
            installButton = button;
        else if (button->text() == "Search" || button->text() == "Restore")
            button->click();
    }
    ASSERT_NE(installButton, nullptr);
    installButton->click();
    EXPECT_TRUE(launcher.launches().empty());
}
} // namespace frost::app
