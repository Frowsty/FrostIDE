// SPDX-License-Identifier: Apache-2.0
//
// FaultVfs: the delegating shim VFS of the storage tests (see fault_vfs.hpp).

#include "support/fault_vfs.hpp"

#include "sqlite_init.hpp"
#include <gtest/gtest.h>

#include <cstdlib>
#include <mutex>

#include <sqlite3.h>

namespace frost::storage::test {

namespace {

// The shim's file: our header, then the real VFS's file (szOsFile = sizeof(ShimFile) + real).
struct ShimFile {
    sqlite3_file base;  // pMethods: kShimMethods, or null when the real open left none.
    sqlite3_file* real; // Points just past this struct.
    int type;           // SQLITE_OPEN_MAIN_DB, ..._WAL, ...
};
static_assert(sizeof(ShimFile) % 8 == 0, "the real file follows the header and needs 8-byte alignment");

sqlite3_vfs* g_real = nullptr; // The platform VFS; set once in instance().

sqlite3_file* realOf(sqlite3_file* file) { return reinterpret_cast<ShimFile*>(file)->real; }
const sqlite3_io_methods& realMethods(sqlite3_file* file) { return *realOf(file)->pMethods; }
int typeOf(sqlite3_file* file) { return reinterpret_cast<ShimFile*>(file)->type; }

int shimClose(sqlite3_file* file) { return realMethods(file).xClose(realOf(file)); }
int shimRead(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
    return realMethods(file).xRead(realOf(file), buffer, amount, offset);
}
int shimWrite(sqlite3_file* file, const void* buffer, int amount, sqlite3_int64 offset) {
    if (const int rc = FaultVfs::instance().check(FaultVfs::Op::Write, typeOf(file)); rc != SQLITE_OK) {
        return rc;
    }
    return realMethods(file).xWrite(realOf(file), buffer, amount, offset);
}
int shimTruncate(sqlite3_file* file, sqlite3_int64 size) {
    if (const int rc = FaultVfs::instance().check(FaultVfs::Op::Truncate, typeOf(file)); rc != SQLITE_OK) {
        return rc;
    }
    return realMethods(file).xTruncate(realOf(file), size);
}
int shimSync(sqlite3_file* file, int flags) {
    if (const int rc = FaultVfs::instance().check(FaultVfs::Op::Sync, typeOf(file)); rc != SQLITE_OK) {
        return rc;
    }
    return realMethods(file).xSync(realOf(file), flags);
}
int shimFileSize(sqlite3_file* file, sqlite3_int64* size) { return realMethods(file).xFileSize(realOf(file), size); }
int shimLock(sqlite3_file* file, int level) { return realMethods(file).xLock(realOf(file), level); }
int shimUnlock(sqlite3_file* file, int level) { return realMethods(file).xUnlock(realOf(file), level); }
int shimCheckReservedLock(sqlite3_file* file, int* out) {
    return realMethods(file).xCheckReservedLock(realOf(file), out);
}
int shimFileControl(sqlite3_file* file, int op, void* arg) {
    return realMethods(file).xFileControl(realOf(file), op, arg);
}
int shimSectorSize(sqlite3_file* file) { return realMethods(file).xSectorSize(realOf(file)); }
int shimDeviceCharacteristics(sqlite3_file* file) { return realMethods(file).xDeviceCharacteristics(realOf(file)); }
int shimShmMap(sqlite3_file* file, int page, int pageSize, int extend, void volatile** out) {
    return realMethods(file).xShmMap(realOf(file), page, pageSize, extend, out);
}
int shimShmLock(sqlite3_file* file, int offset, int n, int flags) {
    return realMethods(file).xShmLock(realOf(file), offset, n, flags);
}
void shimShmBarrier(sqlite3_file* file) { realMethods(file).xShmBarrier(realOf(file)); }
int shimShmUnmap(sqlite3_file* file, int deleteFlag) { return realMethods(file).xShmUnmap(realOf(file), deleteFlag); }
int shimFetch(sqlite3_file* file, sqlite3_int64 offset, int amount, void** out) {
    return realMethods(file).xFetch(realOf(file), offset, amount, out);
}
int shimUnfetch(sqlite3_file* file, sqlite3_int64 offset, void* page) {
    return realMethods(file).xUnfetch(realOf(file), offset, page);
}

// Version 3; the platform VFSes (win32, unix) implement every method of version 3.
constexpr sqlite3_io_methods kShimMethods = {
    3,
    shimClose,
    shimRead,
    shimWrite,
    shimTruncate,
    shimSync,
    shimFileSize,
    shimLock,
    shimUnlock,
    shimCheckReservedLock,
    shimFileControl,
    shimSectorSize,
    shimDeviceCharacteristics,
    shimShmMap,
    shimShmLock,
    shimShmBarrier,
    shimShmUnmap,
    shimFetch,
    shimUnfetch,
};

int vfsOpen(sqlite3_vfs*, sqlite3_filename name, sqlite3_file* file, int flags, int* outFlags) {
    auto* shim = reinterpret_cast<ShimFile*>(file);
    shim->base.pMethods = nullptr;
    shim->real = reinterpret_cast<sqlite3_file*>(shim + 1);
    shim->real->pMethods = nullptr;
    shim->type = flags & 0x000FFF00;
    FaultVfs::instance().recordOpen(name);
    const int rc = g_real->xOpen(g_real, name, shim->real, flags, outFlags);
    // SQLite calls xClose on a failed open only when pMethods is set; mirror the real file.
    shim->base.pMethods = shim->real->pMethods != nullptr ? &kShimMethods : nullptr;
    return rc;
}
int vfsDelete(sqlite3_vfs*, const char* name, int syncDir) { return g_real->xDelete(g_real, name, syncDir); }
int vfsAccess(sqlite3_vfs*, const char* name, int flags, int* out) { return g_real->xAccess(g_real, name, flags, out); }
int vfsFullPathname(sqlite3_vfs*, const char* name, int size, char* out) {
    return g_real->xFullPathname(g_real, name, size, out);
}
void* vfsDlOpen(sqlite3_vfs*, const char* name) { return g_real->xDlOpen(g_real, name); }
void vfsDlError(sqlite3_vfs*, int size, char* out) { g_real->xDlError(g_real, size, out); }
void (*vfsDlSym(sqlite3_vfs*, void* handle, const char* symbol))() { return g_real->xDlSym(g_real, handle, symbol); }
void vfsDlClose(sqlite3_vfs*, void* handle) { g_real->xDlClose(g_real, handle); }
int vfsRandomness(sqlite3_vfs*, int size, char* out) { return g_real->xRandomness(g_real, size, out); }
int vfsSleep(sqlite3_vfs*, int microseconds) { return g_real->xSleep(g_real, microseconds); }
int vfsCurrentTime(sqlite3_vfs*, double* out) { return g_real->xCurrentTime(g_real, out); }
int vfsGetLastError(sqlite3_vfs*, int size, char* out) { return g_real->xGetLastError(g_real, size, out); }
int vfsCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64* out) { return g_real->xCurrentTimeInt64(g_real, out); }
int vfsSetSystemCall(sqlite3_vfs*, const char* name, sqlite3_syscall_ptr call) {
    return g_real->xSetSystemCall(g_real, name, call);
}
sqlite3_syscall_ptr vfsGetSystemCall(sqlite3_vfs*, const char* name) { return g_real->xGetSystemCall(g_real, name); }
const char* vfsNextSystemCall(sqlite3_vfs*, const char* name) { return g_real->xNextSystemCall(g_real, name); }

sqlite3_vfs g_shim{}; // Filled once in instance(); SQLite keeps a pointer to it.

[[noreturn]] void setupFailed(const std::string& why) {
    ADD_FAILURE() << "FaultVfs: " << why;
    std::abort();
}

} // namespace

FaultVfs& FaultVfs::instance() {
    static FaultVfs vfs;
    static std::once_flag registered;
    std::call_once(registered, [] {
        if (auto initialized = detail::initializeSqlite(); !initialized) {
            setupFailed(initialized.error().describe());
        }
#ifdef _WIN32
        g_real = sqlite3_vfs_find("win32-longpath");
#else
        g_real = sqlite3_vfs_find(nullptr);
#endif
        if (g_real == nullptr || g_real->iVersion < 3) {
            setupFailed("no version-3 platform VFS");
        }
        g_shim.iVersion = 3;
        g_shim.szOsFile = static_cast<int>(sizeof(ShimFile)) + g_real->szOsFile;
        g_shim.mxPathname = g_real->mxPathname;
        g_shim.zName = kName;
        g_shim.xOpen = vfsOpen;
        g_shim.xDelete = vfsDelete;
        g_shim.xAccess = vfsAccess;
        g_shim.xFullPathname = vfsFullPathname;
        // Extension loading is compiled out (SQLITE_OMIT_LOAD_EXTENSION): keep the real VFS's nulls.
        g_shim.xDlOpen = g_real->xDlOpen != nullptr ? vfsDlOpen : nullptr;
        g_shim.xDlError = g_real->xDlError != nullptr ? vfsDlError : nullptr;
        g_shim.xDlSym = g_real->xDlSym != nullptr ? vfsDlSym : nullptr;
        g_shim.xDlClose = g_real->xDlClose != nullptr ? vfsDlClose : nullptr;
        g_shim.xRandomness = vfsRandomness;
        g_shim.xSleep = vfsSleep;
        g_shim.xCurrentTime = vfsCurrentTime;
        g_shim.xGetLastError = vfsGetLastError;
        g_shim.xCurrentTimeInt64 = vfsCurrentTimeInt64;
        g_shim.xSetSystemCall = g_real->xSetSystemCall != nullptr ? vfsSetSystemCall : nullptr;
        g_shim.xGetSystemCall = g_real->xGetSystemCall != nullptr ? vfsGetSystemCall : nullptr;
        g_shim.xNextSystemCall = g_real->xNextSystemCall != nullptr ? vfsNextSystemCall : nullptr;
        if (sqlite3_vfs_register(&g_shim, 0) != SQLITE_OK) {
            setupFailed("sqlite3_vfs_register failed");
        }
    });
    return vfs;
}

void FaultVfs::reset() {
    for (std::size_t i = 0; i < 3; ++i) {
        m_counts[i].store(0);
        m_failIndex[i].store(-1);
        m_failCode[i].store(0);
        m_persistent[i].store(false);
    }
    m_typeMask.store(0x7FFFFF00);
    m_fired.store(false);
    std::lock_guard lock(m_namesMutex);
    m_names.clear();
}

void FaultVfs::failAt(Op op, std::int64_t at, int code, bool persistent) {
    m_failCode[index(op)].store(code);
    m_persistent[index(op)].store(persistent);
    m_failIndex[index(op)].store(at);
}

int FaultVfs::check(Op op, int fileType) {
    if ((fileType & m_typeMask.load()) == 0) {
        return SQLITE_OK;
    }
    const std::size_t i = index(op);
    const std::int64_t n = m_counts[i].fetch_add(1);
    const std::int64_t at = m_failIndex[i].load();
    if (at >= 0 && (n == at || (m_persistent[i].load() && n > at))) {
        m_fired.store(true);
        return m_failCode[i].load();
    }
    return SQLITE_OK;
}

void FaultVfs::recordOpen(const char* name) {
    if (name == nullptr) {
        return;
    }
    std::lock_guard lock(m_namesMutex);
    m_names.emplace_back(name);
}

std::vector<std::string> FaultVfs::openedNames() const {
    std::lock_guard lock(m_namesMutex);
    return m_names;
}

} // namespace frost::storage::test
