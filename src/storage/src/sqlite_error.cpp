// SPDX-License-Identifier: Apache-2.0
//
// Private: SQLite result-code mapping and error construction (see sqlite_error.hpp).

#include "sqlite_error.hpp"

#include "frost/io/os_error.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <utility>

namespace frost::storage::detail {

namespace {

// Every result code sqlite3.h 3.54.1 defines, primary and extended, with its macro name.
struct CodeName {
    int code;
    std::string_view name;
};

constexpr std::array kCodeNames{
    CodeName{SQLITE_OK, "SQLITE_OK"},
    CodeName{SQLITE_ERROR, "SQLITE_ERROR"},
    CodeName{SQLITE_INTERNAL, "SQLITE_INTERNAL"},
    CodeName{SQLITE_PERM, "SQLITE_PERM"},
    CodeName{SQLITE_ABORT, "SQLITE_ABORT"},
    CodeName{SQLITE_BUSY, "SQLITE_BUSY"},
    CodeName{SQLITE_LOCKED, "SQLITE_LOCKED"},
    CodeName{SQLITE_NOMEM, "SQLITE_NOMEM"},
    CodeName{SQLITE_READONLY, "SQLITE_READONLY"},
    CodeName{SQLITE_INTERRUPT, "SQLITE_INTERRUPT"},
    CodeName{SQLITE_IOERR, "SQLITE_IOERR"},
    CodeName{SQLITE_CORRUPT, "SQLITE_CORRUPT"},
    CodeName{SQLITE_NOTFOUND, "SQLITE_NOTFOUND"},
    CodeName{SQLITE_FULL, "SQLITE_FULL"},
    CodeName{SQLITE_CANTOPEN, "SQLITE_CANTOPEN"},
    CodeName{SQLITE_PROTOCOL, "SQLITE_PROTOCOL"},
    CodeName{SQLITE_EMPTY, "SQLITE_EMPTY"},
    CodeName{SQLITE_SCHEMA, "SQLITE_SCHEMA"},
    CodeName{SQLITE_TOOBIG, "SQLITE_TOOBIG"},
    CodeName{SQLITE_CONSTRAINT, "SQLITE_CONSTRAINT"},
    CodeName{SQLITE_MISMATCH, "SQLITE_MISMATCH"},
    CodeName{SQLITE_MISUSE, "SQLITE_MISUSE"},
    CodeName{SQLITE_NOLFS, "SQLITE_NOLFS"},
    CodeName{SQLITE_AUTH, "SQLITE_AUTH"},
    CodeName{SQLITE_FORMAT, "SQLITE_FORMAT"},
    CodeName{SQLITE_RANGE, "SQLITE_RANGE"},
    CodeName{SQLITE_NOTADB, "SQLITE_NOTADB"},
    CodeName{SQLITE_NOTICE, "SQLITE_NOTICE"},
    CodeName{SQLITE_WARNING, "SQLITE_WARNING"},
    CodeName{SQLITE_ROW, "SQLITE_ROW"},
    CodeName{SQLITE_DONE, "SQLITE_DONE"},
    CodeName{SQLITE_ERROR_MISSING_COLLSEQ, "SQLITE_ERROR_MISSING_COLLSEQ"},
    CodeName{SQLITE_ERROR_RETRY, "SQLITE_ERROR_RETRY"},
    CodeName{SQLITE_ERROR_SNAPSHOT, "SQLITE_ERROR_SNAPSHOT"},
    CodeName{SQLITE_ERROR_RESERVESIZE, "SQLITE_ERROR_RESERVESIZE"},
    CodeName{SQLITE_ERROR_KEY, "SQLITE_ERROR_KEY"},
    CodeName{SQLITE_ERROR_UNABLE, "SQLITE_ERROR_UNABLE"},
    CodeName{SQLITE_IOERR_READ, "SQLITE_IOERR_READ"},
    CodeName{SQLITE_IOERR_SHORT_READ, "SQLITE_IOERR_SHORT_READ"},
    CodeName{SQLITE_IOERR_WRITE, "SQLITE_IOERR_WRITE"},
    CodeName{SQLITE_IOERR_FSYNC, "SQLITE_IOERR_FSYNC"},
    CodeName{SQLITE_IOERR_DIR_FSYNC, "SQLITE_IOERR_DIR_FSYNC"},
    CodeName{SQLITE_IOERR_TRUNCATE, "SQLITE_IOERR_TRUNCATE"},
    CodeName{SQLITE_IOERR_FSTAT, "SQLITE_IOERR_FSTAT"},
    CodeName{SQLITE_IOERR_UNLOCK, "SQLITE_IOERR_UNLOCK"},
    CodeName{SQLITE_IOERR_RDLOCK, "SQLITE_IOERR_RDLOCK"},
    CodeName{SQLITE_IOERR_DELETE, "SQLITE_IOERR_DELETE"},
    CodeName{SQLITE_IOERR_BLOCKED, "SQLITE_IOERR_BLOCKED"},
    CodeName{SQLITE_IOERR_NOMEM, "SQLITE_IOERR_NOMEM"},
    CodeName{SQLITE_IOERR_ACCESS, "SQLITE_IOERR_ACCESS"},
    CodeName{SQLITE_IOERR_CHECKRESERVEDLOCK, "SQLITE_IOERR_CHECKRESERVEDLOCK"},
    CodeName{SQLITE_IOERR_LOCK, "SQLITE_IOERR_LOCK"},
    CodeName{SQLITE_IOERR_CLOSE, "SQLITE_IOERR_CLOSE"},
    CodeName{SQLITE_IOERR_DIR_CLOSE, "SQLITE_IOERR_DIR_CLOSE"},
    CodeName{SQLITE_IOERR_SHMOPEN, "SQLITE_IOERR_SHMOPEN"},
    CodeName{SQLITE_IOERR_SHMSIZE, "SQLITE_IOERR_SHMSIZE"},
    CodeName{SQLITE_IOERR_SHMLOCK, "SQLITE_IOERR_SHMLOCK"},
    CodeName{SQLITE_IOERR_SHMMAP, "SQLITE_IOERR_SHMMAP"},
    CodeName{SQLITE_IOERR_SEEK, "SQLITE_IOERR_SEEK"},
    CodeName{SQLITE_IOERR_DELETE_NOENT, "SQLITE_IOERR_DELETE_NOENT"},
    CodeName{SQLITE_IOERR_MMAP, "SQLITE_IOERR_MMAP"},
    CodeName{SQLITE_IOERR_GETTEMPPATH, "SQLITE_IOERR_GETTEMPPATH"},
    CodeName{SQLITE_IOERR_CONVPATH, "SQLITE_IOERR_CONVPATH"},
    CodeName{SQLITE_IOERR_VNODE, "SQLITE_IOERR_VNODE"},
    CodeName{SQLITE_IOERR_AUTH, "SQLITE_IOERR_AUTH"},
    CodeName{SQLITE_IOERR_BEGIN_ATOMIC, "SQLITE_IOERR_BEGIN_ATOMIC"},
    CodeName{SQLITE_IOERR_COMMIT_ATOMIC, "SQLITE_IOERR_COMMIT_ATOMIC"},
    CodeName{SQLITE_IOERR_ROLLBACK_ATOMIC, "SQLITE_IOERR_ROLLBACK_ATOMIC"},
    CodeName{SQLITE_IOERR_DATA, "SQLITE_IOERR_DATA"},
    CodeName{SQLITE_IOERR_CORRUPTFS, "SQLITE_IOERR_CORRUPTFS"},
    CodeName{SQLITE_IOERR_IN_PAGE, "SQLITE_IOERR_IN_PAGE"},
    CodeName{SQLITE_IOERR_BADKEY, "SQLITE_IOERR_BADKEY"},
    CodeName{SQLITE_IOERR_CODEC, "SQLITE_IOERR_CODEC"},
    CodeName{SQLITE_LOCKED_SHAREDCACHE, "SQLITE_LOCKED_SHAREDCACHE"},
    CodeName{SQLITE_LOCKED_VTAB, "SQLITE_LOCKED_VTAB"},
    CodeName{SQLITE_BUSY_RECOVERY, "SQLITE_BUSY_RECOVERY"},
    CodeName{SQLITE_BUSY_SNAPSHOT, "SQLITE_BUSY_SNAPSHOT"},
    CodeName{SQLITE_BUSY_TIMEOUT, "SQLITE_BUSY_TIMEOUT"},
    CodeName{SQLITE_CANTOPEN_NOTEMPDIR, "SQLITE_CANTOPEN_NOTEMPDIR"},
    CodeName{SQLITE_CANTOPEN_ISDIR, "SQLITE_CANTOPEN_ISDIR"},
    CodeName{SQLITE_CANTOPEN_FULLPATH, "SQLITE_CANTOPEN_FULLPATH"},
    CodeName{SQLITE_CANTOPEN_CONVPATH, "SQLITE_CANTOPEN_CONVPATH"},
    CodeName{SQLITE_CANTOPEN_DIRTYWAL, "SQLITE_CANTOPEN_DIRTYWAL"},
    CodeName{SQLITE_CANTOPEN_SYMLINK, "SQLITE_CANTOPEN_SYMLINK"},
    CodeName{SQLITE_CORRUPT_VTAB, "SQLITE_CORRUPT_VTAB"},
    CodeName{SQLITE_CORRUPT_SEQUENCE, "SQLITE_CORRUPT_SEQUENCE"},
    CodeName{SQLITE_CORRUPT_INDEX, "SQLITE_CORRUPT_INDEX"},
    CodeName{SQLITE_READONLY_RECOVERY, "SQLITE_READONLY_RECOVERY"},
    CodeName{SQLITE_READONLY_CANTLOCK, "SQLITE_READONLY_CANTLOCK"},
    CodeName{SQLITE_READONLY_ROLLBACK, "SQLITE_READONLY_ROLLBACK"},
    CodeName{SQLITE_READONLY_DBMOVED, "SQLITE_READONLY_DBMOVED"},
    CodeName{SQLITE_READONLY_CANTINIT, "SQLITE_READONLY_CANTINIT"},
    CodeName{SQLITE_READONLY_DIRECTORY, "SQLITE_READONLY_DIRECTORY"},
    CodeName{SQLITE_ABORT_ROLLBACK, "SQLITE_ABORT_ROLLBACK"},
    CodeName{SQLITE_CONSTRAINT_CHECK, "SQLITE_CONSTRAINT_CHECK"},
    CodeName{SQLITE_CONSTRAINT_COMMITHOOK, "SQLITE_CONSTRAINT_COMMITHOOK"},
    CodeName{SQLITE_CONSTRAINT_FOREIGNKEY, "SQLITE_CONSTRAINT_FOREIGNKEY"},
    CodeName{SQLITE_CONSTRAINT_FUNCTION, "SQLITE_CONSTRAINT_FUNCTION"},
    CodeName{SQLITE_CONSTRAINT_NOTNULL, "SQLITE_CONSTRAINT_NOTNULL"},
    CodeName{SQLITE_CONSTRAINT_PRIMARYKEY, "SQLITE_CONSTRAINT_PRIMARYKEY"},
    CodeName{SQLITE_CONSTRAINT_TRIGGER, "SQLITE_CONSTRAINT_TRIGGER"},
    CodeName{SQLITE_CONSTRAINT_UNIQUE, "SQLITE_CONSTRAINT_UNIQUE"},
    CodeName{SQLITE_CONSTRAINT_VTAB, "SQLITE_CONSTRAINT_VTAB"},
    CodeName{SQLITE_CONSTRAINT_ROWID, "SQLITE_CONSTRAINT_ROWID"},
    CodeName{SQLITE_CONSTRAINT_PINNED, "SQLITE_CONSTRAINT_PINNED"},
    CodeName{SQLITE_CONSTRAINT_DATATYPE, "SQLITE_CONSTRAINT_DATATYPE"},
    CodeName{SQLITE_NOTICE_RECOVER_WAL, "SQLITE_NOTICE_RECOVER_WAL"},
    CodeName{SQLITE_NOTICE_RECOVER_ROLLBACK, "SQLITE_NOTICE_RECOVER_ROLLBACK"},
    CodeName{SQLITE_NOTICE_RBU, "SQLITE_NOTICE_RBU"},
    CodeName{SQLITE_WARNING_AUTOINDEX, "SQLITE_WARNING_AUTOINDEX"},
    CodeName{SQLITE_AUTH_USER, "SQLITE_AUTH_USER"},
    CodeName{SQLITE_OK_LOAD_PERMANENTLY, "SQLITE_OK_LOAD_PERMANENTLY"},
    CodeName{SQLITE_OK_SYMLINK, "SQLITE_OK_SYMLINK"},
};

// The OS error mapping of frost::io, for CANTOPEN refinement.
ErrorCode errorCodeFromSystem(int systemErrno) noexcept {
#ifdef _WIN32
    return io::errorCodeFromWin32(static_cast<std::uint32_t>(systemErrno));
#else
    return io::errorCodeFromErrno(systemErrno);
#endif
}

} // namespace

ErrorCode errorCodeFromSqlite(int extendedCode, int systemErrno) noexcept {
    switch (extendedCode) {
    case SQLITE_IOERR_CORRUPTFS:
    case SQLITE_IOERR_DATA: return ErrorCode::Corrupt;
    case SQLITE_READONLY_DBMOVED: return ErrorCode::IoError;
    case SQLITE_CANTOPEN_ISDIR:
    case SQLITE_CANTOPEN_FULLPATH: return ErrorCode::InvalidArgument;
    case SQLITE_CONSTRAINT_UNIQUE:
    case SQLITE_CONSTRAINT_PRIMARYKEY: return ErrorCode::AlreadyExists;
    case SQLITE_ABORT_ROLLBACK: return ErrorCode::Internal;
    default: break;
    }
    switch (extendedCode & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
    case SQLITE_PROTOCOL:
    case SQLITE_SCHEMA: return ErrorCode::Busy;
    case SQLITE_INTERRUPT:
    case SQLITE_ABORT: return ErrorCode::Cancelled;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_FORMAT: return ErrorCode::Corrupt;
    case SQLITE_FULL:
    case SQLITE_IOERR:
    case SQLITE_NOMEM: return ErrorCode::IoError;
    case SQLITE_CANTOPEN: {
        if (systemErrno == 0) {
            return ErrorCode::IoError;
        }
        const ErrorCode refined = errorCodeFromSystem(systemErrno);
        return refined == ErrorCode::Unknown ? ErrorCode::IoError : refined;
    }
    case SQLITE_PERM:
    case SQLITE_READONLY:
    case SQLITE_AUTH: return ErrorCode::PermissionDenied;
    case SQLITE_TOOBIG: return ErrorCode::OutOfRange;
    case SQLITE_CONSTRAINT:
    case SQLITE_MISMATCH:
    case SQLITE_ERROR: return ErrorCode::InvalidArgument;
    case SQLITE_MISUSE:
    case SQLITE_RANGE:
    case SQLITE_INTERNAL:
    case SQLITE_EMPTY:
    case SQLITE_NOTFOUND: return ErrorCode::Internal;
    case SQLITE_NOLFS: return ErrorCode::Unsupported;
    default: return ErrorCode::Unknown;
    }
}

std::string_view sqliteCodeName(int extendedCode) noexcept {
    for (const CodeName& entry : kCodeNames) {
        if (entry.code == extendedCode) {
            return entry.name;
        }
    }
    return "SQLITE_UNKNOWN";
}

Error makeSqliteError(int extendedCode, std::string_view message, int systemErrno, std::string_view operation,
                      std::string_view path, std::string_view sql, std::source_location where) {
    std::string text(operation);
    if (!path.empty()) {
        text += std::format(" '{}'", path);
    }
    text += std::format(": {} (sqlite {} {}", message, extendedCode, sqliteCodeName(extendedCode));
    const int primary = extendedCode & 0xff;
    if (systemErrno != 0 && (primary == SQLITE_CANTOPEN || primary == SQLITE_IOERR)) {
#ifdef _WIN32
        text += std::format(", win32 error {}", static_cast<std::uint32_t>(systemErrno));
#else
        text += std::format(", errno {}", systemErrno);
#endif
    }
    text += ')';
    if (!sql.empty()) {
        // 120 bytes, cut back to a UTF-8 character boundary (our SQL is ASCII, but be safe).
        std::size_t cut = sql.size() < 120 ? sql.size() : 120;
        while (cut < sql.size() && cut > 0 && (static_cast<unsigned char>(sql[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        text += std::format(" [sql: {}{}]", sql.substr(0, cut), cut < sql.size() ? "..." : "");
    }
    return Error(errorCodeFromSqlite(extendedCode, systemErrno), std::move(text), where);
}

Error sqliteError(sqlite3* db, int rc, std::string_view operation, std::string_view path, std::string_view sql,
                  std::source_location where) {
    int extended = rc;
    std::string_view message = sqlite3_errstr(rc);
    int systemErrno = 0;
    if (db != nullptr) {
        const int reported = sqlite3_extended_errcode(db);
        // The connection's state describes this failure only when its primary code agrees.
        if ((reported & 0xff) == (rc & 0xff)) {
            extended = reported;
            message = sqlite3_errmsg(db);
        }
        systemErrno = sqlite3_system_errno(db);
    }
    return makeSqliteError(extended, message, systemErrno, operation, path, sql, where);
}

} // namespace frost::storage::detail
