// SPDX-License-Identifier: Apache-2.0
// Parent identity mismatches and actual retained-process exit observation.
#include <frost/update/parent_process.hpp>
#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
#ifdef _WIN32
std::optional<std::string> numberEnvironment(const wchar_t* name) {
    std::array<wchar_t, 32> buffer{};
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return std::nullopt;
    if (length >= buffer.size())
        return "invalid";
    std::string result;
    for (DWORD index = 0; index < length; ++index) {
        if (buffer[index] < L'0' || buffer[index] > L'9')
            return "invalid";
        result.push_back(static_cast<char>(buffer[index]));
    }
    return result;
}
#endif
TEST(UpdateParent, InvalidIdentityCannotCreateExitBarrier) {
    EXPECT_FALSE(frost::update::ParentProcess::acquire({0, 1}));
    EXPECT_FALSE(frost::update::ParentProcess::acquire({1, 0}));
}
TEST(UpdateParent, IdentityAndMoveKeepTheExactProcess) {
#ifdef _WIN32
    auto identity = frost::update::currentParentIdentity();
    ASSERT_TRUE(identity);
    auto held = frost::update::ParentProcess::acquire(*identity);
    ASSERT_TRUE(held);
    auto moved = std::move(*held);
    EXPECT_FALSE(held->exited());
    auto state = moved.exited();
    ASSERT_TRUE(state);
    EXPECT_FALSE(*state);
    ++identity->creationTime;
    auto wrong = frost::update::ParentProcess::acquire(*identity);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code(), frost::ErrorCode::PermissionDenied);
#else
    EXPECT_EQ(frost::update::currentParentIdentity().error().code(), frost::ErrorCode::Unsupported);
    EXPECT_EQ(frost::update::ParentProcess::acquire({1, 1}).error().code(), frost::ErrorCode::Unsupported);
#endif
}
TEST(UpdateParent, RetainedProcessProbe) {
#ifdef _WIN32
    const auto pidText = numberEnvironment(L"FROST_UPDATE_PARENT_PID");
    const auto creationText = numberEnvironment(L"FROST_UPDATE_PARENT_CREATED");
    if (!pidText || !creationText)
        GTEST_SKIP() << "Invoked by the independent-process parent fixture";
    frost::update::ParentIdentity identity;
    const auto pid = std::string_view(*pidText);
    const auto creation = std::string_view(*creationText);
    const auto parsedPid = std::from_chars(pid.data(), pid.data() + pid.size(), identity.pid);
    const auto parsedCreation =
        std::from_chars(creation.data(), creation.data() + creation.size(), identity.creationTime);
    ASSERT_EQ(parsedPid.ec, std::errc{});
    ASSERT_EQ(parsedPid.ptr, pid.data() + pid.size());
    ASSERT_EQ(parsedCreation.ec, std::errc{});
    ASSERT_EQ(parsedCreation.ptr, creation.data() + creation.size());
    auto held = frost::update::ParentProcess::acquire(identity);
    if (const auto alreadyExited = numberEnvironment(L"FROST_UPDATE_PARENT_ALREADY_EXITED")) {
        ASSERT_EQ(*alreadyExited, "1");
        ASSERT_FALSE(held);
        EXPECT_EQ(held.error().code(), frost::ErrorCode::Cancelled);
        return;
    }
    ASSERT_TRUE(held);
    ASSERT_EQ(held->exited().value(), false);
    std::cout << "PARENT_GUARD_READY" << '\n' << std::flush;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        auto exited = held->exited();
        ASSERT_TRUE(exited);
        if (*exited)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL() << "Retained process exit was not observed";
#else
    GTEST_SKIP() << "Windows installer parent barrier";
#endif
}
} // namespace
