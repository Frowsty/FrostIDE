// SPDX-License-Identifier: Apache-2.0
//
// The part of frost's start-up that both halves of the Windows executable run (ADR 0012):
// frost.exe, the launcher (src/app/src/launcher.cpp), and frost_app.dll, the application it
// loads. It holds the process hardening, the hand-over of its result across the DLL boundary,
// the launcher's DLL search order for the install directory, its rules for telling a GUI start
// from a command-line mode and for when a start may reach a GPU driver, the short link through
// which the launcher restarts frost from a long path (prepareShortLink()), and the path helpers
// they need.
// The source (src/startup_core.cpp) is compiled twice: into frost.exe with the static CRT
// (/MT, every configuration) and into frost::app with the shared one (/MD), because one static
// library cannot serve both (lld-link's /failifmismatch on RuntimeLibrary). So it uses only the
// C++ standard library and Win32: no Qt, no frost::core, nothing that needs another library's
// objects. That is also why prepareShortLink() reports a ShortLinkError, not a frost::Error.
// On Linux there is no launcher; hardenProcess() runs at the top of main() and the
// Windows-only declarations below do not exist (the restart is compiled out there).
#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

/*
 * What hardenProcess() and the launcher applied. Each field is true when its step is in
 * effect: the call succeeded (for safeSearchPathMode, or the safe mode was already
 * permanent), or, for exeSearchSkipsCurrentDirectory, NeedCurrentDirectoryForExePathW
 * confirms that the variable takes effect.
 *
 * Trade-off of the DLL rules, as in Chromium and Firefox: PATH is no longer searched for
 * DLLs, and LoadLibraryExW(bare name, LOAD_WITH_ALTERED_SEARCH_PATH) fails with
 * ERROR_INVALID_PARAMETER. Third-party code loaded into frost (IMEs, shell extensions)
 * that relies on either breaks. A DLL loaded by absolute path with
 * LOAD_WITH_ALTERED_SEARCH_PATH, as COM loads in-process servers and shell extensions,
 * still finds its dependencies in its own directory.
 */
struct ProcessHardening {
    // SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS): LoadLibrary by name, and
    // the dependencies of a DLL loaded by absolute path, are searched only in the
    // application directory, System32 and AddDllDirectory entries. The launcher narrows
    // it further (installDirectoryExtended).
    bool defaultDllDirectories = false;
    // SetDllDirectoryW(L""): removes the current directory from the legacy search orders,
    // including the one LOAD_WITH_ALTERED_SEARCH_PATH uses. While defaultDllDirectories is
    // in effect it is redundant (measured on Windows 11 26300); it is the fallback if
    // SetDefaultDllDirectories fails, and unlike it, any later SetDllDirectoryW call undoes it.
    bool dllDirectoryCleared = false;
    // SetSearchPathMode(safe | permanent): SearchPathW looks in the current directory after
    // the system directories but still before PATH, so a planted tool.exe still wins over
    // the one on PATH. Tool resolution must therefore never use SearchPathW: frost-lint's
    // exe-search rule bans it, and tools resolve to absolute paths (PLAN.md 3.5).
    bool safeSearchPathMode = false;
    // NoDefaultCurrentDirectoryInExePath=1: CreateProcessW with a NULL application name,
    // and cmd.exe in child processes (which inherit the variable), resolve bare executable
    // names without the current directory. It does not affect SearchPathW.
    bool exeSearchSkipsCurrentDirectory = false;
    /*
     * Windows, set by the launcher only (searchInstallDirectoryExtended()): the install
     * directory is an AddDllDirectory entry in its \\?\ form, and the process default search
     * order is LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32 instead of
     * DEFAULT_DIRS. A later LoadLibrary by name of a DLL beside frost.exe then resolves in
     * \\?\ form, which the loader's 250-unit plain-path limit for DLLs embedding RT_MANIFEST
     * resource 2 does not apply to. Under DEFAULT_DIRS the plain application directory is
     * searched first, and its failure (ERROR_FILENAME_EXCED_RANGE) ends the search (measured
     * 2026-11-01 at 300 units; ADR 0012). Both orders exclude the current directory and PATH.
     * False when AddDllDirectory or SetDefaultDllDirectories failed; DEFAULT_DIRS then stays.
     */
    bool installDirectoryExtended = false;

    // True when the four hardenProcess() steps applied (always true on non-Windows
    // platforms). installDirectoryExtended is reported separately.
    [[nodiscard]] bool complete() const noexcept;
};

/*
 * Applies the first four steps above to the whole process. On Windows the launcher calls it
 * first in wmain(), before any DLL other than ntdll, kernel32 and kernelbase is mapped
 * (frost.exe imports only KERNEL32.dll; FrostProcess.LauncherImportsOnlyKernel32 pins that),
 * on the main thread, before any other thread starts: the calls change process-global state
 * that is not synchronised. On Linux main() calls it first. Idempotent.
 * SetDefaultDllDirectories and the permanent search-path mode cannot be undone for the
 * life of the process; the SetDllDirectoryW setting can be, which is harmless while
 * SetDefaultDllDirectories holds.
 * Child processes inherit NoDefaultCurrentDirectoryInExePath through the environment
 * (see the TODO(M2) on ProcessSpec::environment in frost/io/process.hpp); the DLL
 * settings are not inherited.
 * On non-Windows platforms it does nothing and returns a complete state.
 */
[[nodiscard]] ProcessHardening hardenProcess();

// The hardening state as the bit set the launcher passes to frost_main(), and back. Bits
// that no field uses are ignored by decodeHardening(). Pure; O(1).
[[nodiscard]] unsigned encodeHardening(const ProcessHardening& hardening) noexcept;
[[nodiscard]] ProcessHardening decodeHardening(unsigned bits) noexcept;

#ifdef _WIN32

/*
 * frost_app.dll's file name and entry point, the contract between the launcher and the DLL:
 *   extern "C" int frost_main(unsigned hardeningBits, const wchar_t* restartedFrom)
 * receives encodeHardening() of the launcher's state and, when this process is a restart
 * through a short link (prepareShortLink()), the image path of the launcher that restarted it,
 * as that one's GetModuleFileNameW reported it (NUL-terminated, owned by the launcher, valid
 * for the call); nullptr otherwise. The launcher passes it only when the restart marker
 * (kRestartedFromVariable) names this process's own image file (sameImageFile()), so a marker
 * that a parent left or set to another file never arrives here. It is no stronger than the
 * link it came through: same-user tampering is out of scope (ShortLink). Returns the process
 * exit status.
 */
inline constexpr wchar_t kAppDllName[] = L"frost_app.dll";
inline constexpr char kAppEntryPoint[] = "frost_main";
using AppEntryPoint = int (*)(unsigned hardeningBits, const wchar_t* restartedFrom);

/*
 * The image path length, in UTF-16 units (a \\?\ prefix counts), from which a process on this
 * machine cannot load NVIDIA's user-mode graphics driver: MAX_PATH. nvldumdx.dll's DllMain
 * ends the process with a fail-fast (0xC0000409, FAST_FAIL_INVALID_ARG) when the image path is
 * 260 units or longer, whichever DLL asked for the driver: qwindows.dll through d3d9.dll, or a
 * python.exe that only creates a hardware Direct3D 11 device (251 and 255 work, 260 fails), on
 * hidden desktops and on the interactive one alike, with LongPathsEnabled=1 (measured
 * 2026-11-01; ADR 0012). The launcher restarts such a start through a short link
 * (launcherMayReachGpuDriver(), prepareShortLink()).
 */
inline constexpr std::size_t kGpuDriverImagePathLimitUnits = 260;

/*
 * The environment variable that marks a restart: the first launcher sets it to its own image
 * path before it starts frost again through the short link. Every launcher reads it and
 * removes it from its environment before anything else can start a process (so nothing frost
 * starts inherits it). It counts only when it names the reading launcher's own image file
 * (sameImageFile()), as it does after a real restart, since the link leads to the first
 * launcher's directory: that launcher then never restarts again and hands the value to
 * frost_main(). A marker that names anything else is dropped as if it were unset, so a stale
 * or hand-set one neither stops a restart nor makes frost name a directory it does not run from.
 */
inline constexpr wchar_t kRestartedFromVariable[] = L"FROST_RESTARTED_FROM";

/*
 * Whether the image paths `a` and `b` (as modulePath() reports them: plain, \\?\ or \\.\, maybe
 * with 8.3 components, "." or "..") name one file: each is normalised as extendedDirectoryOf()
 * normalises it, opened in that \\?\ form with links followed, and the two volume serial numbers
 * and file IDs are compared (FileIdInfo, 128 bits, so ReFS counts too; GetFileInformationByHandle
 * where a file system has no FileIdInfo). False when either cannot be opened. Thread-safe;
 * two opens and two queries, about 0.1 ms.
 */
[[nodiscard]] bool sameImageFile(const std::wstring& a, const std::wstring& b);

/*
 * How this process's token stands to User Account Control (TOKEN_ELEVATION_TYPE): Default when
 * it has no linked token (a standard user, UAC off, the built-in Administrator, a service), Full
 * for the elevated half of a split token ("Run as administrator"), Limited for the filtered half
 * (an administrator who did not elevate).
 */
enum class UacElevation : unsigned char { Default, Full, Limited };

/*
 * This process's UacElevation: GetTokenInformation(TokenElevationType) of its token. The launcher
 * imports KERNEL32.dll alone, so the function comes from the API set
 * api-ms-win-security-base-l1-1-0.dll, which resolves to the KERNELBASE.dll every process has
 * mapped: no new DLL is loaded. On failure, the win32 error. Thread-safe; about 10 us.
 */
[[nodiscard]] std::expected<UacElevation, unsigned long> processUacElevation();

/*
 * Whether a launcher whose token has `elevation` may restart through a short link: not when it
 * is Full. The link lives in a folder that the same user's programs without elevation can write,
 * and holding it does not stop them from retargeting it (ShortLink), so they could pick the code
 * an elevated frost loads, a privilege that they lack themselves. Pure.
 */
[[nodiscard]] constexpr bool shortLinkAllowedFor(UacElevation elevation) noexcept {
    return elevation != UacElevation::Full;
}

/*
 * Whether the restart may hand this launcher's standard handles to the restarted frost
 * (STARTF_USESTDHANDLES), given the STARTUPINFOW flags `startupFlags` that its parent started it
 * with: not when STARTF_USEHOTKEY or STARTF_USEMONITOR (0x400, which the SDK's headers lack) is
 * set, because hStdInput then holds a hotkey or hStdOutput an HMONITOR (ShellExecuteEx with
 * SEE_MASK_HMONITOR, a start from the taskbar), and neither may be combined with
 * STARTF_USESTDHANDLES. The handle fields then pass as the parent set them. Pure.
 */
[[nodiscard]] bool startupFlagsAllowStandardHandles(unsigned long startupFlags) noexcept;

/*
 * LoadLibraryExW flags for the DLLs the launcher loads by their \\?\ path (frost_app.dll and,
 * in ASan trees, the ASan runtime): LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32
 * (0x900). Their static imports resolve from the DLL's own \\?\ directory and System32 only,
 * never from the plain application directory, the current directory or PATH. frost_app.dll
 * must not carry /DEPENDENTLOADFLAG: a value there replaces these flags for its imports, and
 * 0xA00 (APPLICATION_DIR | SYSTEM32) makes Qt resolve through the plain exe directory, which
 * fails from 236 units on a plain start (measured 2026-11-01; ADR 0012).
 */
inline constexpr unsigned long kLauncherLoadFlags = 0x900;

// The path of `module` (nullptr: this process's executable) as GetModuleFileNameW reports
// it: \\?\-prefixed or with 8.3 short components when it was loaded or started through such
// a path. "" on failure. `module` is an HMODULE. Thread-safe; O(path length).
[[nodiscard]] std::wstring modulePath(const void* module = nullptr);

// `path` without a \\?\ or \\?\UNC\ prefix: the form Qt and other Win32 callers use. Pure.
[[nodiscard]] std::wstring withoutExtendedPrefix(std::wstring path);

// The \\?\ (or \\?\UNC\ for \\server\share) form of the absolute, normalised path `path`,
// which Win32 accepts at any length. Pure.
[[nodiscard]] std::wstring extendedPath(const std::wstring& path);

/*
 * The directory of the executable or DLL at `imagePath` (as modulePath() reports it) as an
 * absolute \\?\ path, the form the launcher loads from. GetModuleFileNameW reports the path as
 * the parent spelled it, and \\?\ takes every character literally, so a plain path is first
 * normalised as Win32 would open it (GetFullPathNameW: "." and ".." components, '/', repeated
 * separators, trailing dots and spaces), a \\.\ device path becomes \\?\ after the same
 * normalisation, and a \\?\ path is kept as it is. 8.3 short components are kept (no file
 * system access). If GetFullPathNameW fails, the spelling is used as given. "" when
 * `imagePath` has no directory part. Thread-safe (GetFullPathNameW reads the current
 * directory only for relative paths, which GetModuleFileNameW does not report); O(length).
 */
[[nodiscard]] std::wstring extendedDirectoryOf(const std::wstring& imagePath);

/*
 * Whether the launcher's arguments (`arguments`: argv without the program name) select a
 * command-line mode, in which no box may appear (tests and scripts run them unattended):
 * true when an argument before the first "--" starts with '-', as frost_app.dll's parser
 * (frost/app/command_line.hpp) reads options; arguments after "--" are paths, so
 * `frost -- -file.txt` is a GUI start. A null element is ignored. Pure; O(arguments).
 */
[[nodiscard]] bool launcherCommandLineMode(std::span<const wchar_t* const> arguments) noexcept;

/*
 * The launcher's step after hardenProcess(): AddDllDirectory(`extendedDirectory`), the
 * install directory in \\?\ form, then SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_USER_DIRS
 * | LOAD_LIBRARY_SEARCH_SYSTEM32). Returns ProcessHardening::installDirectoryExtended. Fails
 * closed: if AddDllDirectory fails, the default order is left at DEFAULT_DIRS and false comes
 * back; the self-test's process-hardening check reports it. Same threading rule as
 * hardenProcess(). Cannot be undone for the life of the process.
 */
[[nodiscard]] bool searchInstallDirectoryExtended(const std::wstring& extendedDirectory);

/*
 * Whether a start with the launcher's `arguments` (argv without the program name) and the
 * QT_QPA_PLATFORM value `qtQpaPlatform` may load a GPU driver, that is, creates a
 * QApplication on a platform other than offscreen, as main.cpp decides: the GUI (no option
 * before "--") and the self-test on a native platform. The self-test is offscreen unless it
 * says "--selftest=onscreen" or QT_QPA_PLATFORM is set; a QT_QPA_PLATFORM whose first named
 * entry (platformPluginNames()' rule: ';'-separated, ':' options dropped, empty entries
 * skipped) is "offscreen" keeps any mode offscreen. --version, --help and usage errors (any
 * other option) create no QApplication. A null element is ignored. Pure; O(arguments + value).
 */
[[nodiscard]] bool launcherMayReachGpuDriver(std::span<const wchar_t* const> arguments,
                                             std::wstring_view qtQpaPlatform) noexcept;

/*
 * The short link's directory name for the install directory `finalDirectory` (a plain path as
 * GetFinalPathNameByHandleW names it, so every spelling of one directory gives one name): 16
 * lower-case hex digits of the 64-bit FNV-1a hash of its UTF-16 units (little-endian bytes).
 * Stable across runs and builds. Pure; O(length).
 */
[[nodiscard]] std::wstring shortLinkName(std::wstring_view finalDirectory);

/*
 * The REPARSE_DATA_BUFFER bytes of a mount point (junction) to the plain absolute drive path
 * `target`, for FSCTL_SET_REPARSE_POINT: tag IO_REPARSE_TAG_MOUNT_POINT, substitute name
 * "\??\<target>", print name `target`, both NUL-terminated as the I/O manager expects. Empty
 * when the data would exceed MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 KiB, a target of about 4,090
 * units). Pure; O(length).
 */
[[nodiscard]] std::vector<unsigned char> mountPointReparseData(std::wstring_view target);

// The substitute name of reparse data `data` (FSCTL_GET_REPARSE_POINT's output) if it is a
// well-formed mount point, else "" (another tag, truncated, or names outside the buffer). Pure.
[[nodiscard]] std::wstring mountPointSubstituteName(std::span<const unsigned char> data);

/*
 * A verified short link: the junction <linkRoot>\FrostIDE\i\<shortLinkName()> to frost's
 * install directory, held open (FILE_FLAG_OPEN_REPARSE_POINT, GENERIC_READ, FILE_SHARE_READ
 * only) for as long as the object lives; the launcher keeps it until the restarted frost exits.
 * What the hold guarantees: sharing modes refuse every other opener that asks for write-data,
 * append-data or delete access (GENERIC_WRITE, DELETE: ERROR_SHARING_VIOLATION), so while it is
 * open nobody can remove or rename the junction, nor rename a folder above it. Readers, such as
 * a second frost verifying the same link, share it.
 * What it does not guarantee: sharing modes never check FILE_WRITE_ATTRIBUTES, WRITE_DAC or
 * WRITE_OWNER, and FILE_WRITE_ATTRIBUTES is all that FSCTL_SET_REPARSE_POINT and
 * FSCTL_DELETE_REPARSE_POINT need. So whoever the folder's ACL lets write the link (the user's
 * own programs, SYSTEM, administrators) can still retarget or strip it at any time: between its
 * verification and the restart, and while the restarted frost loads its DLLs through it. That
 * is same-user tampering, which prepareShortLink() leaves out of scope; an elevated launcher
 * never restarts through a link (shortLinkAllowedFor()). Both halves are measured (2026-11-01,
 * StartupShortLink.HoldStopsRemovalButNotAReparseDataWriter). Move-only; closes the handle on
 * destruction.
 */
class ShortLink {
public:
    ShortLink(std::wstring directory, std::wstring imagePath, std::wstring target, void* handle) noexcept;
    ~ShortLink();
    ShortLink(ShortLink&& other) noexcept;
    ShortLink& operator=(ShortLink&& other) noexcept;
    ShortLink(const ShortLink&) = delete ("owns the open handle that pins the link");
    ShortLink& operator=(const ShortLink&) = delete ("owns the open handle that pins the link");

    // The junction's plain path as the file system resolves it (GetFinalPathNameByHandleW of
    // the held handle), e.g. C:\Users\u\AppData\Local\FrostIDE\i\0123456789abcdef: no folder
    // in it is a reparse point, so it names the held junction for as long as the object lives.
    [[nodiscard]] const std::wstring& directory() const noexcept { return m_directory; }
    // directory() joined with the launcher's image file name: what the restart starts.
    [[nodiscard]] const std::wstring& imagePath() const noexcept { return m_imagePath; }
    // The install directory the junction points at, as GetFinalPathNameByHandleW names it (plain).
    [[nodiscard]] const std::wstring& target() const noexcept { return m_target; }

private:
    std::wstring m_directory;
    std::wstring m_imagePath;
    std::wstring m_target;
    void* m_handle = nullptr; // HANDLE; the header avoids <windows.h>.
};

// Why prepareShortLink() gave no link. `message` completes the sentence "frost restarts through
// a short link, but ..." (no trailing period).
struct ShortLinkError {
    enum class Kind : unsigned char {
        Unusable, // The location or the install directory cannot have such a link (too long,
                  // not absolute, a network drive or share, an install directory of more
                  // than about 4,089 units for a junction's 16 KiB of reparse data).
        Occupied, // Something frost does not remove is at the link's path: a file, a
                  // directory with contents, or a reparse point that holds data.
        Failed,   // A Win32 call failed (the message names it), or the path kept changing.
    };
    Kind kind = Kind::Failed;
    std::wstring message;
};

/*
 * Finds, creates or repairs the short link to `installDirectory` (the launcher's \\?\ install
 * directory, extendedDirectoryOf() of its image path) under `linkRoot` (an absolute drive path;
 * the launcher passes %LOCALAPPDATA%), for an image named `imageName`, and returns it held open.
 *   1. The target is the install directory as the file system names it (GetFinalPathNameByHandleW:
 *      8.3 components, letter case and links resolved), so every spelling gets one link; a target
 *      on a network share or mapped drive is Unusable (a junction can point at local volumes
 *      only), and so is one of more than about 4,089 units (mountPointReparseData()). `linkRoot`
 *      must start with a drive letter, a colon and a separator, or it is Unusable; it is then
 *      normalised as Win32 opens a plain path (GetFullPathNameW: "." and "..", '/', repeated
 *      separators, trailing dots and spaces), because the \\?\ form used below takes every
 *      character literally, and on a network drive it is Unusable. The link's image path must
 *      stay below kGpuDriverImagePathLimitUnits, or Unusable says how long it would be.
 *   2. <linkRoot>\FrostIDE and its i subfolder are created when missing, with the ACL they
 *      inherit; nothing checks their owner or ACL. Under the real %LOCALAPPDATA% that admits the
 *      user, SYSTEM and Administrators only, so no other standard user can write there; a
 *      `linkRoot` that other users may write (a parent can point LOCALAPPDATA anywhere) would let
 *      them plant or change the link.
 *   3. The link path is opened without following it (FILE_FLAG_OPEN_REPARSE_POINT) and inspected
 *      through that handle: a mount point whose substitute name is "\??\<target>" (ordinal,
 *      case-insensitive) is kept as it is; a mount point to anything else, or an empty plain
 *      directory, is rewritten in place (FSCTL_SET_REPARSE_POINT with IO_REPARSE_TAG_MOUNT_POINT,
 *      no administrator rights needed); another name-surrogate reparse point (a symbolic link of
 *      either kind, a WSL symlink) is deleted itself, never its target, and created anew. A file,
 *      a directory with contents and a reparse point that is no name surrogate (cloud files,
 *      dedup, app execution aliases: deleting those deletes data) are Occupied and left untouched.
 *      After every change the link is opened and verified again; a sharing violation or a
 *      pending delete (another frost setting the link up) is retried for up to about 1 s.
 *   4. The verified link is named by its resolved path (ShortLink::directory()), so a junction
 *      or symbolic link above it (in `linkRoot` itself, say) cannot be retargeted between the
 *      check and the restart; that path must be on a local drive and its image path below
 *      kGpuDriverImagePathLimitUnits too, or Unusable.
 * Same-user tampering is out of scope: a process running as the user can change the link at any
 * time, even while a frost holds it (ShortLink), as it could replace frost.exe itself.
 * Thread-safe; blocking file-system calls only. When the link exists it costs two opens and two
 * GetFinalPathNameByHandleW (the install directory and the link), one GetFullPathNameW, up to
 * three GetDriveTypeW, three CreateDirectoryW that find their directory, and one attribute query
 * and reparse read of the link; creating it adds about 0.6 ms (ADR 0012, Measurements).
 * TODO(M2): links whose install directory is gone stay behind (an empty junction each); the
 * installer's uninstall, or a sweep when frost has a settings store, removes them.
 */
[[nodiscard]] std::expected<ShortLink, ShortLinkError>
prepareShortLink(std::wstring_view linkRoot, const std::wstring& installDirectory, std::wstring_view imageName);

#endif

} // namespace frost::app
