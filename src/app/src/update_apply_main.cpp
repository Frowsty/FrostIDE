// SPDX-License-Identifier: Apache-2.0
// Detached Windows updater. The private pipes carry a verified release and the restart decision.
#include <frost/io/file_system.hpp>
#include <frost/io/path.hpp>
#include <frost/io/process.hpp>
#include <frost/io/utf.hpp>
#include <frost/update/parent_process.hpp>
#include <frost/update/payload_guard.hpp>
#include <frost/update/safe_updater.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace frost;
using namespace std::chrono_literals;

template<typename T>
bool readNumber(std::string_view value, T& result, int base = 10) {
    if (value.empty())
        return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

template<std::size_t N>
std::string_view readLine(std::array<char, N>& buffer) {
    if (!std::cin.getline(buffer.data(), static_cast<std::streamsize>(buffer.size())) || std::cin.eof())
        return {};
    return {buffer.data(), static_cast<std::size_t>(std::cin.gcount() - 1)};
}

Result<update::ParentIdentity> readIdentity() {
    std::array<char, 64> storage{};
    const auto line = readLine(storage);
    const auto separator = line.find(' ');
    update::ParentIdentity identity{};
    if (separator == line.npos || !readNumber(line.substr(0, separator), identity.pid) ||
        !readNumber(line.substr(separator + 1), identity.creationTime))
        return fail(ErrorCode::InvalidArgument, "Invalid update parent identity");
    return identity;
}

Result<update::Release> readRelease() {
    std::array<char, 32> sizeStorage{};
    std::array<char, 4096> floorStorage{};
    update::Release release{};
    if (!readNumber(readLine(sizeStorage), release.size) || release.size == 0)
        return fail(ErrorCode::InvalidArgument, "Invalid update payload size");
    auto floor = update::parseReleaseFloor(readLine(floorStorage));
    if (!floor)
        return std::unexpected(floor.error());
    release.version = std::move(floor->version);
    release.build = floor->build;
    const std::string_view digest = floor->digest;
    for (std::size_t i = 0; i < release.digest.size(); ++i) {
        if (!readNumber(digest.substr(i * 2, 2), release.digest[i], 16))
            return fail(ErrorCode::InvalidArgument, "Invalid update payload digest");
    }
    return release;
}

Result<void> waitForConfirmedExit(update::ParentProcess& parent) {
    const auto deadline = std::chrono::steady_clock::now() + 45s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto stopped = parent.exited();
        if (!stopped)
            return std::unexpected(stopped.error());
        if (*stopped)
            return {};
        std::this_thread::sleep_for(25ms);
    }
    return fail(ErrorCode::Timeout, "FrostIDE did not exit before the update deadline");
}

Result<update::UpdateOutcome> apply(const wchar_t* installPath, const wchar_t* backupPath, const wchar_t* payloadPath,
                                    bool& authorized) {
    const auto install = io::fromWide(installPath);
    const auto backup = io::fromWide(backupPath);
    const auto payload = io::fromWide(payloadPath);
    if (!install || !backup || !payload || !io::isAbsolutePath(*install) || !io::isAbsolutePath(*backup) ||
        !io::isAbsolutePath(*payload))
        return fail(ErrorCode::InvalidArgument, "Updater paths must be absolute Windows paths");

    const auto identity = readIdentity();
    if (!identity)
        return std::unexpected(identity.error());
    auto parent = update::ParentProcess::acquire(*identity);
    if (!parent)
        return std::unexpected(parent.error());
    auto release = readRelease();
    if (!release)
        return std::unexpected(release.error());
    std::array<char, 66> receiptStorage{};
    const auto receiptDigest = readLine(receiptStorage);
    if (receiptDigest.size() != 64 || receiptDigest.find_first_not_of("0123456789abcdef") != receiptDigest.npos)
        return fail(ErrorCode::InvalidArgument, "Invalid installed updater receipt digest");
    auto payloadGuard = update::PayloadGuard::acquire(std::filesystem::path(payloadPath), *release);
    if (!payloadGuard)
        return std::unexpected(payloadGuard.error());

    std::cout << "ready\n" << std::flush;
    std::array<char, 16> decision{};
    if (readLine(decision) != "apply" || std::cin.peek() != std::char_traits<char>::eof())
        return fail(ErrorCode::Cancelled, "The in-place update was not authorized");
    authorized = true;
    if (auto stopped = waitForConfirmedExit(*parent); !stopped)
        return std::unexpected(stopped.error());
    if (auto verified = payloadGuard->validate(); !verified)
        return std::unexpected(verified.error());

    update::SafeUpdateSpec spec;
    spec.installDir = *install;
    spec.backupDir = *backup;
    spec.payloadPath = *payload;
    spec.expectedRelease = std::move(*release);
    spec.expectedInstallReceiptDigest = receiptDigest;
    spec.launchDetachedAfterInstall = true;
    spec.healthCheckTimeout = 30s;
    auto& fs = io::realFileSystem();
    auto& launcher = io::realProcessLauncher();
    auto outcome = update::SafeUpdater(fs, launcher).applyUpdate(spec);
    if (outcome && outcome->rolledBack) {
        io::ProcessSpec restart;
        restart.executable = outcome->activeExecutablePath;
        restart.workingDirectory = *install;
        restart.killOnClose = false;
        if (auto started = launcher.launch(restart); !started)
            return std::unexpected(started.error());
    }
    return outcome;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4)
        return 2;
    bool authorized = false;
    const auto result = apply(argv[1], argv[2], argv[3], authorized);
    const auto message = !result ? result.error().describe()
                                 : (result->applied ? "Update installed" : result->failureReason + "; rolled back");
    if (authorized) {
        auto receipt = frost::io::fromStdPath(std::filesystem::path(argv[0]).parent_path() / L"update-result.txt");
        if (receipt)
            (void)frost::io::realFileSystem().writeFileAtomic(*receipt, message + "\n");
    }
    if (!result) {
        std::cerr << message << '\n';
        return 3;
    }
    std::cout << (result->applied ? "applied" : "rolled-back") << '\n';
    return result->applied ? 0 : 4;
}
