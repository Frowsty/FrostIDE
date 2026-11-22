// SPDX-License-Identifier: Apache-2.0
// Offline current-user private records and real local stdio proxy; no provider/Claude process.
#include "frost/io/file_system.hpp"
#include "frost/io/local_ipc.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/json.hpp"

#include "../../src/app/src/agent_private.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>
#include <QUuid>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
namespace {
using frost::rpc::Json;
TEST(AgentPrivateFile, CreateNewNeverOverwritesAndReaderRejectsHardLinks) {
    QTemporaryDir temp;
    const auto directory = QDir(temp.path()).filePath("private").toStdString();
    ASSERT_TRUE(frost::app::ensureAgentPrivateDirectory(directory));
    const auto path = directory + "/fixture.discovery.json";
    ASSERT_TRUE(frost::app::writeAgentPrivateFile(path, "private fixture"));
    EXPECT_FALSE(frost::app::writeAgentPrivateFile(path, "replacement"));
    const auto read = frost::app::readAgentPrivateFile(path);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, "private fixture");
    const auto source = frost::io::toStdPath(path);
    const auto alias = frost::io::toStdPath(directory + "/alias.discovery.json");
    ASSERT_TRUE(source);
    ASSERT_TRUE(alias);
    std::error_code error;
    std::filesystem::create_hard_link(*source, *alias, error);
    ASSERT_FALSE(error) << error.message();
    EXPECT_FALSE(frost::app::readAgentPrivateFile(path));
    EXPECT_FALSE(frost::app::readAgentPrivateFile(directory + "/alias.discovery.json"));
}
TEST(AgentPrivateFile, ReaderRejectsOversizedRecords) {
    QTemporaryDir temp;
    const auto directory = QDir(temp.path()).filePath("private").toStdString();
    ASSERT_TRUE(frost::app::ensureAgentPrivateDirectory(directory));
    EXPECT_FALSE(frost::app::writeAgentPrivateFile(directory + "/too-large.discovery.json", std::string(65537, 'x')));
}
TEST(AgentMcpProxy, RealStdioUsesCurrentUserPipeAndExactWindowToken) {
    using namespace std::chrono_literals;
    QTemporaryDir temp;
    auto& fs = frost::io::realFileSystem();
    const auto root = fs.physicalPath(temp.path().toStdString());
    ASSERT_TRUE(root);
    const auto directory = QDir(temp.path()).filePath("private").toStdString();
    ASSERT_TRUE(frost::app::ensureAgentPrivateDirectory(directory));
    const auto key =
        QCryptographicHash::hash(QUuid::createUuid().toRfc4122(), QCryptographicHash::Sha256).toHex().toStdString();
    const std::string token(32, 'a'), instance(32, 'b');
    auto owner = frost::io::LocalIpcOwner::claim(key);
    ASSERT_TRUE(owner);
    ASSERT_TRUE((*owner)->start([&](const std::string& bytes) -> frost::Result<std::string> {
        auto envelope = frost::rpc::parseJson(bytes);
        if (!envelope || envelope->stringAt("token") != token || envelope->stringAt("instance") != instance ||
            envelope->stringAt("root") != *root)
            return frost::fail(frost::ErrorCode::PermissionDenied, "fixture token rejected");
        const auto* request = envelope->find("request");
        if (!request)
            return frost::fail(frost::ErrorCode::InvalidArgument, "missing fixture request");
        const auto result = request->stringAt("method") == "notifications/poll"
                                ? Json::object().set("revision", 0).set("notifications", Json::array())
                                : Json::object().set("fixture", true);
        return Json::object()
            .set("jsonrpc", "2.0")
            .set("id", request->find("id") ? *request->find("id") : Json())
            .set("result", result)
            .dump();
    }));
    const auto expiry =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() +
        3600;
    const auto discovery = directory + "/fixture.discovery.json";
    const auto record = Json::object()
                            .set("kind", "frostide-mcp")
                            .set("root", *root)
                            .set("key", key)
                            .set("token", token)
                            .set("instance", instance)
                            .set("expires", expiry);
    ASSERT_TRUE(frost::app::writeAgentPrivateFile(discovery, record.dump()));
    frost::io::ProcessSpec spec;
    spec.executable = FROST_MCP_PROXY_PATH;
    spec.arguments = {"--discovery", discovery, "--root", *root};
    spec.stdinMode = frost::io::Stdio::Pipe;
    auto response = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec,
                                               "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"initialize\"}\n", 10s);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->exitCode, 0) << response->stderrBytes;
    EXPECT_NE(response->stdoutBytes.find("fixture"), std::string::npos);
    // Omitted --root must resolve the proxy's actual cwd through Result<string> conversion.
    spec.arguments = {"--discovery", discovery};
    spec.workingDirectory = *root;
    response = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec,
                                          "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/list\"}\n", 10s);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->exitCode, 0) << response->stderrBytes;
    EXPECT_NE(response->stdoutBytes.find("fixture"), std::string::npos);
    const auto wrong = directory + "/wrong.discovery.json";
    ASSERT_TRUE(frost::app::writeAgentPrivateFile(wrong, Json(record).set("token", std::string(32, 'c')).dump()));
    spec.arguments = {"--discovery", wrong, "--root", *root};
    response = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec,
                                          "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/list\"}\n", 10s);
    ASSERT_TRUE(response);
    EXPECT_NE(response->exitCode, 0);
    EXPECT_TRUE(response->stdoutBytes.empty());
}
} // namespace
