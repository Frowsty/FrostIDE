// SPDX-License-Identifier: Apache-2.0
// Offline native panel construction: no Connect, provider process, prompt or tool execution.
#include "../../src/app/src/native_agent_chat.hpp"
#include "workbench_rig.hpp"
#include <QComboBox>
#include <QDialog>
namespace {
TEST(NativeAgentChat, CreatesTheCheckpointSelectorWithoutStartingASession) {
    frost::app::test::WorkDir dir("native-checkpoint-ui");
    struct ConfigEnvironment {
        bool existed = qEnvironmentVariableIsSet("CLAUDE_CONFIG_DIR");
        QByteArray previous = qgetenv("CLAUDE_CONFIG_DIR");
        ~ConfigEnvironment() { if (existed) qputenv("CLAUDE_CONFIG_DIR", previous); else qunsetenv("CLAUDE_CONFIG_DIR"); }
    } environment;
    qputenv("CLAUDE_CONFIG_DIR", QByteArray::fromStdString(dir.join("isolated-cli-config")));
    frost::app::test::WorkbenchRig rig(dir);
    frost::app::NativeAgentChat chat(*rig.workbench);
    chat.show();
    auto* panel = rig.window->findChild<QDialog*>(QStringLiteral("nativeAgentChatPanel"));
    ASSERT_NE(panel, nullptr);
    auto* selector = panel->findChild<QComboBox*>(QStringLiteral("nativeAgentCheckpoints"));
    ASSERT_NE(selector, nullptr);
    EXPECT_EQ(selector->count(), 0); // No synthetic checkpoint is offered before an actual CLI event.
    EXPECT_FALSE(selector->accessibleName().isEmpty());
    EXPECT_FALSE(rig.workbench->agentBridgeActive());
}
}
