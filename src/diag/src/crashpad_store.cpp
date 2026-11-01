// SPDX-License-Identifier: Apache-2.0
//
// The retention seams over the real system (frost/diag/retention.hpp): crashpad's report
// database (crashpad::CrashReportDatabase, Apache-2.0) and the active-file probe.

#include "frost/diag/retention.hpp"

#include <client/crash_report_database.h>
#include <util/misc/uuid.h>

#include <chrono>
#include <ctime>

#ifdef _WIN32
#include "frost/io/path.hpp"
#else
#include <sys/file.h>

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace frost::diag {
namespace {

class CrashpadReportStore final : public ICrashReportStore {
public:
    explicit CrashpadReportStore(std::unique_ptr<crashpad::CrashReportDatabase> database) noexcept
        : m_database(std::move(database)) {}

    Result<std::vector<Report>> list() override {
        std::vector<crashpad::CrashReportDatabase::Report> pending;
        std::vector<crashpad::CrashReportDatabase::Report> completed;
        if (m_database->GetPendingReports(&pending) != crashpad::CrashReportDatabase::kNoError ||
            m_database->GetCompletedReports(&completed) != crashpad::CrashReportDatabase::kNoError) {
            return fail(ErrorCode::IoError, "cannot list the crash report database");
        }
        std::vector<Report> reports;
        for (const auto* list : {&pending, &completed}) {
            for (const auto& report : *list) {
                reports.push_back({report.uuid.ToString(), report.total_size,
                                   IClock::SystemTime(std::chrono::duration_cast<IClock::Duration>(
                                       std::chrono::seconds(report.creation_time)))});
            }
        }
        return reports;
    }

    Result<void> remove(const std::string& id) override {
        crashpad::UUID uuid;
        if (!uuid.InitializeFromString(std::string_view(id))) {
            return fail(ErrorCode::InvalidArgument, "not a report id: " + id);
        }
        switch (m_database->DeleteReport(uuid)) {
        case crashpad::CrashReportDatabase::kNoError: return {};
        case crashpad::CrashReportDatabase::kReportNotFound: return fail(ErrorCode::NotFound, id);
        case crashpad::CrashReportDatabase::kBusyError: return fail(ErrorCode::Busy, id);
        default: return fail(ErrorCode::IoError, "cannot delete crash report " + id);
        }
    }

    void clean(IClock::Duration olderThan) noexcept override {
        (void)m_database->CleanDatabase(
            static_cast<time_t>(std::chrono::duration_cast<std::chrono::seconds>(olderThan).count()));
    }

private:
    std::unique_ptr<crashpad::CrashReportDatabase> m_database;
};

void destroyStore(ICrashReportStore* store) { delete static_cast<CrashpadReportStore*>(store); }

class RealActiveFileProbe final : public IActiveFileProbe {
public:
    constexpr RealActiveFileProbe() noexcept = default;

    bool isActive([[maybe_unused]] const std::string& path) noexcept override {
#ifdef _WIN32
        return false; // The writer's share mode makes the deletion fail with Busy instead.
#else
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            return false;
        }
        const bool active = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
        ::close(fd); // Releases the probe's lock, if it got one.
        return active;
#endif
    }
};

constinit RealActiveFileProbe g_probe;

} // namespace

Result<std::unique_ptr<ICrashReportStore, void (*)(ICrashReportStore*)>>
openCrashpadReportStore(const std::string& path) {
#ifdef _WIN32
    // The \\?\ form, as the handler's --database gets it, so a long cache root works too.
    auto wide = io::toWin32Path(path);
    if (!wide) {
        return std::unexpected(std::move(wide.error()));
    }
    const base::FilePath databasePath(*wide);
#else
    const base::FilePath databasePath(path);
#endif
    auto database = crashpad::CrashReportDatabase::Initialize(databasePath);
    if (!database) {
        return fail(ErrorCode::IoError, "cannot open the crash report database at " + path);
    }
    return std::unique_ptr<ICrashReportStore, void (*)(ICrashReportStore*)>(
        new CrashpadReportStore(std::move(database)), &destroyStore);
}

IActiveFileProbe& realActiveFileProbe() noexcept { return g_probe; }

} // namespace frost::diag
