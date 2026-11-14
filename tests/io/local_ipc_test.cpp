// SPDX-License-Identifier: Apache-2.0
// Real local IPC owner election, startup race, framing, timeout and restart.
#include "frost/io/local_ipc.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;
std::string key() {
    static std::atomic<std::uint64_t> serial{0};
    return std::format(
        "{:064x}", static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) + serial++);
}
// Connect as this user without the production framing code, to inject broken client frames.
#ifdef _WIN32
using Raw = HANDLE;
Raw invalidRaw() { return INVALID_HANDLE_VALUE; }
std::wstring objectStem(const std::string& id) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> data(size);
    if (!size || !GetTokenInformation(token, TokenUser, data.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
        return {};
    std::wstring stem = L"FrostIDE-" + std::wstring(sid) + L"-" + std::wstring(id.begin(), id.end());
    LocalFree(sid);
    return stem;
}
Raw rawConnect(const std::string& id) {
    const std::wstring stem = objectStem(id);
    if (stem.empty())
        return invalidRaw();
    std::wstring name = L"\\\\.\\pipe\\" + stem;
    for (unsigned i = 0; i < 100; ++i) {
        Raw result = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (result != invalidRaw())
            return result;
        std::this_thread::sleep_for(10ms);
    }
    return invalidRaw();
}
void expectCurrentUserOnly(Raw raw);
TEST(LocalIpc, GlobalOwnerAndPipeShareTheUserProjectNamespace) {
    HANDLE token = nullptr;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    TOKEN_ELEVATION elevation{};
    DWORD used = 0;
    const bool queried = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &used) != FALSE;
    CloseHandle(token);
    ASSERT_TRUE(queried);
    if (elevation.TokenIsElevated)
        GTEST_SKIP() << "requires an unelevated token";
    const std::string id = key();
    const std::wstring stem = objectStem(id);
    ASSERT_FALSE(stem.empty());
    auto owner = frost::io::LocalIpcOwner::claim(id);
    ASSERT_TRUE(owner.has_value()) << owner.error().describe();
    HANDLE global = OpenMutexW(SYNCHRONIZE | READ_CONTROL, FALSE, (L"Global\\" + stem).c_str());
    ASSERT_NE(global, nullptr);
    expectCurrentUserOnly(global);
    CloseHandle(global);
    HANDLE local = OpenMutexW(SYNCHRONIZE, FALSE, (L"Local\\" + stem).c_str());
    EXPECT_EQ(local, nullptr);
    if (local)
        CloseHandle(local);
    ASSERT_TRUE((*owner)->start([](std::string value) -> frost::Result<std::string> { return value; }));
    Raw pipe = rawConnect(id);
    ASSERT_NE(pipe, invalidRaw());
    CloseHandle(pipe);
}
void expectCurrentUserOnly(Raw raw) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    ASSERT_EQ(GetSecurityInfo(raw, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                              &descriptor),
              ERROR_SUCCESS);
    ASSERT_NE(dacl, nullptr);
    ACL_SIZE_INFORMATION info{};
    ASSERT_TRUE(GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation));
    EXPECT_EQ(info.AceCount, 1U);
    void* rawAce = nullptr;
    ASSERT_TRUE(GetAce(dacl, 0, &rawAce));
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
    EXPECT_EQ(ace->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
    HANDLE token = nullptr;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD size = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> data(size);
    ASSERT_TRUE(GetTokenInformation(token, TokenUser, data.data(), size, &size));
    EXPECT_TRUE(EqualSid(&ace->SidStart, reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid));
    CloseHandle(token);
    LocalFree(descriptor);
}
void rawSend(Raw raw, const char* data, std::size_t size) {
    DWORD written = 0;
    (void)WriteFile(raw, data, static_cast<DWORD>(size), &written, nullptr);
}
void rawClose(Raw raw) { CloseHandle(raw); }
#else
using Raw = int;
Raw invalidRaw() { return -1; }
Raw rawConnect(const std::string& id) {
    const std::string directory = "/tmp/frostide-ipc-" + std::to_string(geteuid());
    struct stat status{};
    EXPECT_EQ(lstat(directory.c_str(), &status), 0);
    EXPECT_EQ(status.st_uid, geteuid());
    EXPECT_EQ(status.st_mode & 077, 0U);
    const std::string path = directory + "/" + id;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return invalidRaw();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.data(), path.size());
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return invalidRaw();
    }
    return fd;
}
void rawSend(Raw raw, const char* data, std::size_t size) { (void)send(raw, data, size, MSG_NOSIGNAL); }
void rawClose(Raw raw) { close(raw); }
#endif

TEST(LocalIpc, MalformedAndIncompleteFramesDoNotReachHandler) {
    const auto id = key();
    auto owner = frost::io::LocalIpcOwner::claim(id);
    ASSERT_TRUE(owner.has_value());
    std::atomic<unsigned> calls{0};
    ASSERT_TRUE((*owner)->start([&](std::string value) -> frost::Result<std::string> {
        ++calls;
        return value;
    }));
    Raw malformed = rawConnect(id);
    ASSERT_NE(malformed, invalidRaw());
#ifdef _WIN32
    expectCurrentUserOnly(malformed);
#endif
    const std::array<char, 4> huge{char(0xff), char(0xff), char(0xff), char(0xff)};
    rawSend(malformed, huge.data(), huge.size());
    rawClose(malformed);
    // The next legitimate caller succeeds even after a malformed oversized frame.
    auto ok = frost::io::requestLocalIpc(id, "next", 2s);
    ASSERT_TRUE(ok.has_value()) << ok.error().describe();
    EXPECT_EQ(*ok, "next");
    EXPECT_EQ(calls.load(), 1U);
    Raw incomplete = rawConnect(id);
    ASSERT_NE(incomplete, invalidRaw());
    const std::array<char, 2> partial{4, 0};
    rawSend(incomplete, partial.data(), partial.size());
    std::this_thread::sleep_for(150ms); // Let the worker enter its bounded read.
    const auto begin = std::chrono::steady_clock::now();
    owner->reset(); // A connected client that never finishes cannot prevent shutdown indefinitely.
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 4s);
    EXPECT_EQ(calls.load(), 1U);
    rawClose(incomplete);
    auto replacement = frost::io::LocalIpcOwner::claim(id);
    ASSERT_TRUE(replacement.has_value());
}

TEST(LocalIpc, OwnerElectionStartupRaceAndRestart) {
    const auto id = key();
    auto owner = frost::io::LocalIpcOwner::claim(id);
    ASSERT_TRUE(owner.has_value());
    auto other = frost::io::LocalIpcOwner::claim(id);
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().code(), frost::ErrorCode::Busy);
    frost::Result<std::string> reply = frost::fail(frost::ErrorCode::Unknown, "not called");
    std::jthread client([&] { reply = frost::io::requestLocalIpc(id, "hello", 3s); });
    std::this_thread::sleep_for(100ms);
    ASSERT_TRUE((*owner)->start([](const std::string& text) -> frost::Result<std::string> { return text + " world"; }));
    client.join();
    ASSERT_TRUE(reply.has_value()) << reply.error().describe();
    EXPECT_EQ(*reply, "hello world");
    EXPECT_FALSE(frost::io::requestLocalIpc(id, std::string(65537, 'x'), 1s).has_value());
    owner->reset();
    auto replacement = frost::io::LocalIpcOwner::claim(id);
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE((*replacement)->start([](const std::string&) -> frost::Result<std::string> {
        return frost::fail(frost::ErrorCode::InvalidArgument, "wrong project");
    }));
    auto refused = frost::io::requestLocalIpc(id, "other", 2s);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().message(), "wrong project");
}
TEST(LocalIpc, MaximumPayloadRoundTripsAndOversizedReplyIsAnError) {
    const auto id = key();
    auto owner = frost::io::LocalIpcOwner::claim(id);
    ASSERT_TRUE(owner.has_value());
    ASSERT_TRUE((*owner)->start([](std::string message) -> frost::Result<std::string> {
        if (message == "oversized")
            return std::string(65537, 'x');
        return message;
    }));
    const std::string maximum(65536, 'x');
    auto result = frost::io::requestLocalIpc(id, maximum, 5s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(*result, maximum);
    auto tooLarge = frost::io::requestLocalIpc(id, "oversized", 5s);
    ASSERT_FALSE(tooLarge.has_value());
    EXPECT_EQ(tooLarge.error().message(), "IPC reply exceeds size limit");
}
TEST(LocalIpc, KilledOwnerCanBeReplacedWithoutStaleEndpointRace) {
    const auto id = key();
    frost::io::ProcessSpec spec;
    spec.executable = frost::io::normalizePath(FROST_LOCAL_IPC_TEST_CHILD).value();
    spec.arguments = {id};
    spec.stdoutMode = frost::io::Stdio::Null;
    spec.stderrMode = frost::io::Stdio::Null;
    auto child = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(child.has_value());
    auto ready = frost::io::requestLocalIpc(id, "ready", 5s);
    ASSERT_TRUE(ready.has_value()) << ready.error().describe();
    ASSERT_TRUE((*child)->terminate().has_value());
    ASSERT_TRUE((*child)->wait(5s).has_value());
    auto owner = frost::io::LocalIpcOwner::claim(id);
    ASSERT_TRUE(owner.has_value()) << owner.error().describe();
    ASSERT_TRUE((*owner)->start([](std::string value) -> frost::Result<std::string> { return value; }));
    auto reply = frost::io::requestLocalIpc(id, "recovered", 2s);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(*reply, "recovered");
}
TEST(LocalIpc, MissingOwnerIsBoundedAndKeysAreValidated) {
    auto invalid = frost::io::LocalIpcOwner::claim("../x");
    EXPECT_FALSE(invalid.has_value());
    const auto begin = std::chrono::steady_clock::now();
    auto result = frost::io::requestLocalIpc(key(), "hello", 100ms);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
}
} // namespace
