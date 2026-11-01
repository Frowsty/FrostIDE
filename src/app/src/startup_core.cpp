// SPDX-License-Identifier: Apache-2.0
//
// Implementation of frost/app/startup_core.hpp: process hardening, the launcher's
// install-directory search order, its GPU-driver rule and the short link it restarts through.
// Compiled into frost.exe (static CRT) and into frost::app (shared CRT), so it uses only the
// standard library and Win32 (see the header).

#include "frost/app/startup_core.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <winioctl.h>
#endif

namespace frost::app {
namespace {

// The bit of each ProcessHardening field in encodeHardening()'s set.
constexpr unsigned kDefaultDllDirectoriesBit = 1u << 0;
constexpr unsigned kDllDirectoryClearedBit = 1u << 1;
constexpr unsigned kSafeSearchPathModeBit = 1u << 2;
constexpr unsigned kExeSearchSkipsCurrentDirectoryBit = 1u << 3;
constexpr unsigned kInstallDirectoryExtendedBit = 1u << 4;

} // namespace

bool ProcessHardening::complete() const noexcept {
#ifdef _WIN32
    return defaultDllDirectories && dllDirectoryCleared && safeSearchPathMode && exeSearchSkipsCurrentDirectory;
#else
    return true;
#endif
}

ProcessHardening hardenProcess() {
    ProcessHardening state;
#ifdef _WIN32
    state.defaultDllDirectories = SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) != FALSE;
    state.dllDirectoryCleared = SetDllDirectoryW(L"") != FALSE;
    // Repeating the same permanent mode succeeds. Any other mode after a permanent one fails
    // with ERROR_ACCESS_DENIED, and since PERMANENT can only be combined with safe mode,
    // that failure also means safe mode is in force.
    state.safeSearchPathMode =
        SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT) != FALSE ||
        GetLastError() == ERROR_ACCESS_DENIED;
    // Children inherit the variable; whether user build scripts keep it is decided per
    // launch (the TODO(M2) on ProcessSpec::environment in frost/io/process.hpp).
    SetEnvironmentVariableW(L"NoDefaultCurrentDirectoryInExePath", L"1");
    state.exeSearchSkipsCurrentDirectory = NeedCurrentDirectoryForExePathW(L"frost-probe.exe") == FALSE;
#endif
    return state;
}

unsigned encodeHardening(const ProcessHardening& hardening) noexcept {
    unsigned bits = 0;
    bits |= hardening.defaultDllDirectories ? kDefaultDllDirectoriesBit : 0u;
    bits |= hardening.dllDirectoryCleared ? kDllDirectoryClearedBit : 0u;
    bits |= hardening.safeSearchPathMode ? kSafeSearchPathModeBit : 0u;
    bits |= hardening.exeSearchSkipsCurrentDirectory ? kExeSearchSkipsCurrentDirectoryBit : 0u;
    bits |= hardening.installDirectoryExtended ? kInstallDirectoryExtendedBit : 0u;
    return bits;
}

ProcessHardening decodeHardening(unsigned bits) noexcept {
    ProcessHardening hardening;
    hardening.defaultDllDirectories = (bits & kDefaultDllDirectoriesBit) != 0;
    hardening.dllDirectoryCleared = (bits & kDllDirectoryClearedBit) != 0;
    hardening.safeSearchPathMode = (bits & kSafeSearchPathModeBit) != 0;
    hardening.exeSearchSkipsCurrentDirectory = (bits & kExeSearchSkipsCurrentDirectoryBit) != 0;
    hardening.installDirectoryExtended = (bits & kInstallDirectoryExtendedBit) != 0;
    return hardening;
}

#ifdef _WIN32

std::wstring modulePath(const void* module) {
    // The longest path Windows supports, with its NUL.
    constexpr std::size_t kMaxUnits = 32768;
    // HMODULE is a pointer to the module's base; the header avoids <windows.h>.
    auto* handle = static_cast<HMODULE>(const_cast<void*>(module));
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(handle, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        if (path.size() >= kMaxUnits) {
            return {};
        }
        path.resize(std::min(path.size() * 2, kMaxUnits));
    }
}

std::wstring withoutExtendedPrefix(std::wstring path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        return L"\\\\" + path.substr(8);
    }
    if (path.starts_with(L"\\\\?\\")) {
        return path.substr(4);
    }
    return path;
}

std::wstring extendedPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

std::wstring extendedDirectoryOf(const std::wstring& imagePath) {
    std::wstring path = imagePath;
    if (!path.starts_with(L"\\\\?\\")) {
        // GetFullPathNameW normalises after a \\.\ prefix too, and keeps the prefix.
        if (const DWORD units = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr); units > 0) {
            std::wstring full(units, L'\0');
            const DWORD written = GetFullPathNameW(path.c_str(), units, full.data(), nullptr);
            if (written > 0 && written < units) {
                full.resize(written);
                path = std::move(full);
            }
        }
        if (path.starts_with(L"\\\\.\\")) {
            path[2] = L'?';
        } else {
            path = extendedPath(path);
        }
    }
    const std::size_t slash = path.find_last_of(L'\\');
    // "\\?\" itself is no directory.
    return slash == std::wstring::npos || slash < 4 ? std::wstring() : path.substr(0, slash);
}

bool launcherCommandLineMode(std::span<const wchar_t* const> arguments) noexcept {
    for (const wchar_t* argument : arguments) {
        if (argument == nullptr) {
            continue;
        }
        if (argument[0] == L'-' && argument[1] == L'-' && argument[2] == L'\0') {
            return false;
        }
        if (argument[0] == L'-') {
            return true;
        }
    }
    return false;
}

bool searchInstallDirectoryExtended(const std::wstring& extendedDirectory) {
    if (AddDllDirectory(extendedDirectory.c_str()) == nullptr) {
        return false;
    }
    // DEFAULT_DIRS is APPLICATION_DIR | SYSTEM32 | USER_DIRS; the \\?\ user directory just added
    // stands in for the plain application directory, which the loader searches first and which
    // ends the search with ERROR_FILENAME_EXCED_RANGE on a long plain start.
    return SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32) != FALSE;
}

bool launcherMayReachGpuDriver(std::span<const wchar_t* const> arguments, std::wstring_view qtQpaPlatform) noexcept {
    bool selfTest = false;
    bool onScreen = false;
    for (const wchar_t* argument : arguments) {
        if (argument == nullptr) {
            continue;
        }
        const std::wstring_view option = argument;
        if (option == L"--") {
            break;
        }
        if (!option.starts_with(L'-')) {
            continue;
        }
        if (option == L"--selftest") {
            selfTest = true;
        } else if (option == L"--selftest=onscreen") {
            selfTest = true;
            onScreen = true;
        } else {
            // --version, --help, or a usage error (an unknown option, or one that conflicts
            // with --selftest): frost_app.dll prints and exits without a QApplication.
            return false;
        }
    }
    // main.cpp sets QT_QPA_PLATFORM=offscreen for --selftest only while the variable is empty.
    if (qtQpaPlatform.empty()) {
        return !selfTest || onScreen;
    }
    // The first named entry, as platformPluginNames() reads it; none names Qt's native default.
    std::wstring_view rest = qtQpaPlatform;
    while (true) {
        const std::size_t end = rest.find(L';');
        std::wstring_view entry = rest.substr(0, end);
        entry = entry.substr(0, entry.find(L':'));
        if (!entry.empty()) {
            return entry != L"offscreen";
        }
        if (end == std::wstring_view::npos) {
            return true;
        }
        rest.remove_prefix(end + 1);
    }
}

namespace {

/*
 * 64-bit FNV-1a over the UTF-16 units of `text`, low byte first. FNV is by Glenn Fowler,
 * Landon Curt Noll and Kiem-Phong Vo and is in the public domain
 * (http://www.isthe.com/chongo/tech/comp/fnv/); offset basis and prime as published there.
 */
std::uint64_t fnv1a64(std::wstring_view text) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const wchar_t unit : text) {
        for (const unsigned byte : {static_cast<unsigned>(unit) & 0xFFu, (static_cast<unsigned>(unit) >> 8) & 0xFFu}) {
            hash ^= byte;
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

/*
 * REPARSE_DATA_BUFFER's mount-point form, from the DDK's ntifs.h, which the user-mode SDK does
 * not ship: DWORD ReparseTag, WORD ReparseDataLength, WORD Reserved (the 8-byte header), then
 * WORD SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength (byte
 * offsets and lengths into PathBuffer, without NULs) and the WCHAR PathBuffer.
 */
constexpr std::size_t kReparseHeaderBytes = 8;
constexpr std::size_t kMountPointFieldsBytes = 8;
// MAXIMUM_REPARSE_DATA_BUFFER_SIZE (winnt.h).
constexpr std::size_t kMaximumReparseBytes = 16 * 1024;

template<typename T>
void store(std::vector<unsigned char>& out, std::size_t at, T value) noexcept {
    std::memcpy(out.data() + at, &value, sizeof value);
}

template<typename T>
T load(std::span<const unsigned char> in, std::size_t at) noexcept {
    T value{};
    std::memcpy(&value, in.data() + at, sizeof value);
    return value;
}

} // namespace

std::wstring shortLinkName(std::wstring_view finalDirectory) {
    constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::uint64_t hash = fnv1a64(finalDirectory);
    std::wstring name(16, L'0');
    for (std::size_t i = name.size(); i-- > 0; hash >>= 4) {
        name[i] = kDigits[hash & 0xFu];
    }
    return name;
}

std::vector<unsigned char> mountPointReparseData(std::wstring_view target) {
    const std::wstring substitute = L"\\??\\" + std::wstring(target);
    const std::size_t substituteBytes = substitute.size() * sizeof(wchar_t);
    const std::size_t printBytes = target.size() * sizeof(wchar_t);
    // Both names NUL-terminated, as mklink and the I/O manager write them.
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    const std::size_t total = kReparseHeaderBytes + kMountPointFieldsBytes + pathBytes;
    if (total > kMaximumReparseBytes) {
        return {};
    }
    std::vector<unsigned char> data(total, 0);
    store<DWORD>(data, 0, IO_REPARSE_TAG_MOUNT_POINT);
    store<WORD>(data, 4, static_cast<WORD>(kMountPointFieldsBytes + pathBytes));
    store<WORD>(data, 8, 0);
    store<WORD>(data, 10, static_cast<WORD>(substituteBytes));
    store<WORD>(data, 12, static_cast<WORD>(substituteBytes + sizeof(wchar_t)));
    store<WORD>(data, 14, static_cast<WORD>(printBytes));
    const std::size_t path = kReparseHeaderBytes + kMountPointFieldsBytes;
    std::memcpy(data.data() + path, substitute.data(), substituteBytes);
    std::memcpy(data.data() + path + substituteBytes + sizeof(wchar_t), target.data(), printBytes);
    return data;
}

std::wstring mountPointSubstituteName(std::span<const unsigned char> data) {
    constexpr std::size_t kPath = kReparseHeaderBytes + kMountPointFieldsBytes;
    if (data.size() < kPath || load<DWORD>(data, 0) != IO_REPARSE_TAG_MOUNT_POINT) {
        return {};
    }
    const std::size_t declared = kReparseHeaderBytes + load<WORD>(data, 4);
    if (declared > data.size() || declared < kPath) {
        return {};
    }
    const std::size_t offset = load<WORD>(data, 8);
    const std::size_t length = load<WORD>(data, 10);
    if (offset % sizeof(wchar_t) != 0 || length % sizeof(wchar_t) != 0 || kPath + offset + length > declared) {
        return {};
    }
    std::wstring name(length / sizeof(wchar_t), L'\0');
    std::memcpy(name.data(), data.data() + kPath + offset, length);
    return name;
}

ShortLink::ShortLink(std::wstring directory, std::wstring imagePath, std::wstring target, void* handle) noexcept
    : m_directory(std::move(directory)), m_imagePath(std::move(imagePath)), m_target(std::move(target)),
      m_handle(handle) {}

ShortLink::~ShortLink() {
    if (m_handle != nullptr) {
        CloseHandle(m_handle);
    }
}

ShortLink::ShortLink(ShortLink&& other) noexcept
    : m_directory(std::move(other.m_directory)), m_imagePath(std::move(other.m_imagePath)),
      m_target(std::move(other.m_target)), m_handle(std::exchange(other.m_handle, nullptr)) {}

ShortLink& ShortLink::operator=(ShortLink&& other) noexcept {
    if (this != &other) {
        if (m_handle != nullptr) {
            CloseHandle(m_handle);
        }
        m_directory = std::move(other.m_directory);
        m_imagePath = std::move(other.m_imagePath);
        m_target = std::move(other.m_target);
        m_handle = std::exchange(other.m_handle, nullptr);
    }
    return *this;
}

namespace {

// FormatMessageW's text for a Win32 error, without the trailing period and line break.
std::wstring systemMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD units =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = units > 0 && buffer != nullptr ? std::wstring(buffer, units) : std::wstring(L"unknown error");
    LocalFree(buffer);
    while (!text.empty() &&
           (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    return text;
}

std::unexpected<ShortLinkError> refuse(ShortLinkError::Kind kind, std::wstring message) {
    return std::unexpected(ShortLinkError{kind, std::move(message)});
}

// "<what>: <system text> (win32 error N)" as a Failed error.
std::unexpected<ShortLinkError> failed(const std::wstring& what, DWORD error) {
    return refuse(ShortLinkError::Kind::Failed,
                  what + L": " + systemMessage(error) + L" (win32 error " + std::to_wstring(error) + L")");
}

// Whether `path` starts with a drive letter, a colon and a backslash.
bool isDriveAbsolute(std::wstring_view path) noexcept {
    return path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
           path[1] == L':' && path[2] == L'\\';
}

bool isRemoteDrive(std::wstring_view path) {
    const std::wstring root(path.substr(0, 3));
    return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}

/*
 * The install directory as the file system names it (GetFinalPathNameByHandleW with
 * FILE_NAME_NORMALIZED | VOLUME_NAME_DOS, without its \\?\ prefix): the junction's target and
 * the input of shortLinkName().
 */
// GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) of `handle`, with its \\?\
// prefix, at any length; "" on failure, with GetLastError() saying why.
std::wstring finalPathOf(HANDLE handle) {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD units = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
                                                      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (units == 0) {
            return {};
        }
        if (units < path.size()) {
            path.resize(units);
            return path;
        }
        path.resize(units + 1);
    }
}

std::expected<std::wstring, ShortLinkError> finalDirectoryPath(const std::wstring& directory) {
    HANDLE handle =
        CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return failed(L"cannot open the install directory " + withoutExtendedPrefix(directory), error);
    }
    std::wstring path = finalPathOf(handle);
    const DWORD error = GetLastError();
    CloseHandle(handle);
    if (path.empty()) {
        return failed(L"cannot resolve the install directory " + withoutExtendedPrefix(directory), error);
    }
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        return refuse(ShortLinkError::Kind::Unusable, L"the install directory " + withoutExtendedPrefix(path) +
                                                          L" is on a network share, which a junction cannot point to");
    }
    path = withoutExtendedPrefix(std::move(path));
    if (!isDriveAbsolute(path)) {
        return refuse(ShortLinkError::Kind::Unusable,
                      L"the install directory " + path + L" is on a volume without a drive letter");
    }
    if (isRemoteDrive(path)) {
        return refuse(ShortLinkError::Kind::Unusable,
                      L"the install directory " + path + L" is on a network drive, which a junction cannot point to");
    }
    return path;
}

// What is at the link's path, as seen through a handle that does not follow it.
enum class LinkState : unsigned char {
    Ours,             // A mount point to the install directory.
    Rewritable,       // A mount point to anything else, or a plain directory (maybe empty).
    RemovableLink,    // Another name-surrogate reparse point: a symbolic link, a WSL symlink.
    File,             // A file that is no name surrogate.
    DataReparsePoint, // A reparse point that is no name surrogate (its data would go with it).
};

std::expected<LinkState, ShortLinkError> inspect(HANDLE handle, const std::wstring& link,
                                                 std::wstring_view expectedSubstitute, DWORD& tag) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof info) == FALSE) {
        const DWORD error = GetLastError();
        return failed(L"cannot read the attributes of " + link, error);
    }
    tag = info.ReparseTag;
    const bool directory = (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return directory ? LinkState::Rewritable : LinkState::File;
    }
    if (tag != IO_REPARSE_TAG_MOUNT_POINT || !directory) {
        return IsReparseTagNameSurrogate(tag) ? LinkState::RemovableLink : LinkState::DataReparsePoint;
    }
    std::vector<unsigned char> data(kMaximumReparseBytes);
    DWORD returned = 0;
    if (DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, data.data(), static_cast<DWORD>(data.size()),
                        &returned, nullptr) == FALSE) {
        const DWORD error = GetLastError();
        return failed(L"cannot read the junction " + link, error);
    }
    data.resize(returned);
    const std::wstring substitute = mountPointSubstituteName(data);
    const bool same =
        !substitute.empty() &&
        CompareStringOrdinal(substitute.data(), static_cast<int>(substitute.size()), expectedSubstitute.data(),
                             static_cast<int>(expectedSubstitute.size()), TRUE) == CSTR_EQUAL;
    return same ? LinkState::Ours : LinkState::Rewritable;
}

// Opens the link itself, never what it points at.
HANDLE openLink(const std::wstring& link, DWORD access, DWORD share) {
    return CreateFileW(link.c_str(), access, share, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

// Errors that mean another opener (a second frost setting the link up, an indexer) holds the
// link or a delete of it is pending: worth another attempt.
bool transient(DWORD error) noexcept {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_DELETE_PENDING;
}

// How often and how long prepareShortLink() retries a transient error: about 1 s in total.
constexpr int kLinkAttempts = 100;
constexpr DWORD kLinkRetryMilliseconds = 10;

} // namespace

std::expected<ShortLink, ShortLinkError>
prepareShortLink(std::wstring_view linkRoot, const std::wstring& installDirectory, std::wstring_view imageName) {
    auto target = finalDirectoryPath(installDirectory);
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    std::wstring root(linkRoot);
    while (root.size() > 3 && root.back() == L'\\') {
        root.pop_back();
    }
    if (!isDriveAbsolute(root)) {
        return refuse(ShortLinkError::Kind::Unusable,
                      L"the folder for its short links, \"" + root + L"\", is not an absolute path on a drive");
    }
    if (isRemoteDrive(root)) {
        return refuse(ShortLinkError::Kind::Unusable,
                      L"the folder for its short links, " + root + L", is on a network drive");
    }
    const std::wstring frostFolder = root + (root.back() == L'\\' ? L"" : L"\\") + L"FrostIDE";
    const std::wstring parent = frostFolder + L"\\i";
    const std::wstring link = parent + L"\\" + shortLinkName(*target);
    std::wstring image = link + L"\\" + std::wstring(imageName);
    if (image.size() >= kGpuDriverImagePathLimitUnits) {
        return refuse(ShortLinkError::Kind::Unusable, L"its short link " + image + L" would still be " +
                                                          std::to_wstring(image.size()) + L" UTF-16 units long");
    }
    const std::vector<unsigned char> data = mountPointReparseData(*target);
    if (data.empty()) {
        return refuse(ShortLinkError::Kind::Unusable, L"the install directory's path is too long for a junction");
    }
    // The file operations use the \\?\ form, so a root near the limit works without long paths too.
    for (const std::wstring& folder : {frostFolder, parent}) {
        if (CreateDirectoryW(extendedPath(folder).c_str(), nullptr) == FALSE) {
            if (const DWORD error = GetLastError(); error != ERROR_ALREADY_EXISTS) {
                return failed(L"cannot create " + folder, error);
            }
        }
    }

    const std::wstring linkExtended = extendedPath(link);
    const std::wstring expected = L"\\??\\" + *target;
    bool created = false;
    DWORD lastError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kLinkAttempts; ++attempt) {
        if (attempt > 0 && transient(lastError)) {
            Sleep(kLinkRetryMilliseconds);
        }
        lastError = ERROR_SUCCESS;
        if (CreateDirectoryW(linkExtended.c_str(), nullptr) != FALSE) {
            created = true;
        } else if (const DWORD error = GetLastError(); error != ERROR_ALREADY_EXISTS) {
            if (transient(error)) {
                lastError = error;
                continue;
            }
            return failed(L"cannot create " + link, error);
        }

        // Inspect and keep through a reader that denies write and delete access to others.
        HANDLE reader = openLink(linkExtended, GENERIC_READ, FILE_SHARE_READ);
        if (reader == INVALID_HANDLE_VALUE) {
            lastError = GetLastError();
            if (transient(lastError)) {
                continue;
            }
            return failed(L"cannot open " + link, lastError);
        }
        DWORD tag = 0;
        auto state = inspect(reader, link, expected, tag);
        if (state && *state == LinkState::Ours) {
            /*
             * The restart goes through the held link's own resolved path, not through the
             * root as spelled: a folder above the link may be a junction or symbolic link
             * that its owner could retarget before CreateProcessW, but in the resolved path
             * no folder is a reparse point, and none can be renamed while the reader is open.
             */
            const std::wstring resolved = finalPathOf(reader);
            const DWORD error = GetLastError();
            if (resolved.empty()) {
                CloseHandle(reader);
                return failed(L"cannot resolve " + link, error);
            }
            std::wstring directory = withoutExtendedPrefix(resolved);
            if (resolved.starts_with(L"\\\\?\\UNC\\") || !isDriveAbsolute(directory) || isRemoteDrive(directory)) {
                CloseHandle(reader);
                return refuse(ShortLinkError::Kind::Unusable,
                              L"its short link " + link + L" resolves to " + directory + L", not a local drive");
            }
            std::wstring resolvedImage = directory + L"\\" + std::wstring(imageName);
            if (resolvedImage.size() >= kGpuDriverImagePathLimitUnits) {
                CloseHandle(reader);
                return refuse(ShortLinkError::Kind::Unusable, L"its short link " + resolvedImage + L" would still be " +
                                                                  std::to_wstring(resolvedImage.size()) +
                                                                  L" UTF-16 units long");
            }
            return ShortLink(std::move(directory), std::move(resolvedImage), std::move(*target), reader);
        }
        CloseHandle(reader);
        if (!state) {
            return std::unexpected(std::move(state.error()));
        }
        switch (*state) {
        case LinkState::Ours: break;
        case LinkState::File:
            return refuse(ShortLinkError::Kind::Occupied,
                          L"its short link's path " + link + L" holds a file; remove it and start frost again");
        case LinkState::DataReparsePoint:
            return refuse(ShortLinkError::Kind::Occupied, std::format(L"its short link's path {} holds a reparse point "
                                                                      L"with data (tag {:#x}); remove it and start "
                                                                      L"frost again",
                                                                      link, tag));
        case LinkState::Rewritable: {
            // Reopened for writing, and inspected again, since it may have changed meanwhile.
            HANDLE writer = openLink(linkExtended, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ);
            if (writer == INVALID_HANDLE_VALUE) {
                lastError = GetLastError();
                if (transient(lastError)) {
                    continue;
                }
                return failed(L"cannot open " + link + L" for writing", lastError);
            }
            auto again = inspect(writer, link, expected, tag);
            DWORD error = ERROR_SUCCESS;
            if (again && *again == LinkState::Rewritable) {
                DWORD returned = 0;
                if (DeviceIoControl(writer, FSCTL_SET_REPARSE_POINT, const_cast<unsigned char*>(data.data()),
                                    static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr) == FALSE) {
                    error = GetLastError();
                }
            }
            CloseHandle(writer);
            if (!again) {
                return std::unexpected(std::move(again.error()));
            }
            if (error == ERROR_DIR_NOT_EMPTY) {
                return refuse(ShortLinkError::Kind::Occupied, L"its short link's path " + link +
                                                                  L" is a directory with contents; remove it and "
                                                                  L"start frost again");
            }
            if (error != ERROR_SUCCESS) {
                if (created) {
                    RemoveDirectoryW(linkExtended.c_str());
                }
                return failed(L"cannot make " + link + L" a junction to " + *target, error);
            }
            break;
        }
        case LinkState::RemovableLink: {
            // Deletes the link object itself: the handle does not follow it.
            HANDLE remover = openLink(linkExtended, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ);
            if (remover == INVALID_HANDLE_VALUE) {
                lastError = GetLastError();
                if (transient(lastError)) {
                    continue;
                }
                return failed(L"cannot open " + link + L" to remove it", lastError);
            }
            auto again = inspect(remover, link, expected, tag);
            DWORD error = ERROR_SUCCESS;
            if (again && *again == LinkState::RemovableLink) {
                FILE_DISPOSITION_INFO disposition{};
                disposition.DeleteFile = TRUE;
                if (SetFileInformationByHandle(remover, FileDispositionInfo, &disposition, sizeof disposition) ==
                    FALSE) {
                    error = GetLastError();
                }
            }
            CloseHandle(remover);
            if (!again) {
                return std::unexpected(std::move(again.error()));
            }
            if (error != ERROR_SUCCESS) {
                return failed(L"cannot remove the link " + link, error);
            }
            break;
        }
        }
    }
    return refuse(
        ShortLinkError::Kind::Failed,
        L"its short link " + link + L" kept changing while frost set it up" +
            (lastError != ERROR_SUCCESS ? L" (last win32 error " + std::to_wstring(lastError) + L")" : std::wstring()));
}

#endif

} // namespace frost::app
