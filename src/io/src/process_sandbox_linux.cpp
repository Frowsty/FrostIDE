// SPDX-License-Identifier: Apache-2.0
// Prepares Linux sandbox launches and enforces filesystem restrictions with Landlock.
#include "process_sandbox_internal.hpp"
#include "frost/io/os_error.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/openat2.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace frost::io {
Result<ProcessSpec> stageWindowsSandboxProcess(const ProcessSpec&, std::span<const SandboxStagingFile>, std::string_view) {
    return fail(ErrorCode::Unsupported, "Windows private LPAC staging is unavailable on this platform");
}
namespace detail {
Result<ProcessSpec> prepareLinuxSandboxLaunch(const ProcessSpec& original) {
#if defined(__linux__) && defined(SYS_landlock_create_ruleset)
    if (auto valid = validateProcessSandbox(original, false); !valid) return std::unexpected(valid.error());
    const auto abi = syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 6) return fail(ErrorCode::Unsupported, "Extension sandbox requires enabled Linux Landlock ABI 6 or newer; no unsandboxed fallback");
    std::array<char, 4096> self{};
    const auto count = readlink("/proc/self/exe", self.data(), self.size());
    if (count <= 0 || static_cast<std::size_t>(count) >= self.size()) return fail(ErrorCode::Unsupported, "Cannot identify owned sandbox helper location");
    std::string helper(self.data(), static_cast<std::size_t>(count));
    const auto slash = helper.rfind('/'); if (slash == helper.npos) return fail(ErrorCode::Unsupported, "Invalid owned helper directory");
    helper.resize(slash + 1); helper += "frost-extension-sandbox";
    struct stat status{};
    if (lstat(helper.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || (status.st_mode & 0111) == 0)
        return fail(ErrorCode::Unsupported, "Owned frost-extension-sandbox helper is not installed; no unsandboxed fallback");
    ProcessSpec spec = original;
    spec.executable = helper;
    spec.arguments = {"--profile-v1", original.sandbox->network == SandboxNetwork::Deny ? "deny-network" : "internet-client"};
    for (const auto& grant : original.sandbox->paths) {
        spec.arguments.push_back(std::to_string(static_cast<unsigned>(grant.access)));
        spec.arguments.push_back(grant.recursive ? "tree" : "exact"); spec.arguments.push_back(grant.path);
    }
    spec.arguments.push_back("--"); spec.arguments.push_back(original.executable);
    spec.arguments.insert(spec.arguments.end(), original.arguments.begin(), original.arguments.end());
    // The helper's loader must not receive attacker-selected preload/search modules before it can
    // restrict itself. Target and helper share the explicitly supplied safe environment.
    for (const auto& [name, value] : original.environment) {
        (void)value;
        if (name.starts_with("LD_") || name.starts_with("DYLD_") || name.starts_with("MALLOC_") ||
            name == "GLIBC_TUNABLES" || name == "GCONV_PATH" || name == "LOCPATH" || name == "NLSPATH" || name == "LIBPATH" || name == "SHLIB_PATH")
            return fail(ErrorCode::InvalidArgument, "Sandbox environment cannot change the owned helper loader: " + name);
    }
    spec.environment.emplace_back("LC_ALL", "C");
    spec.sandbox.reset(); // Only the owned enforcing helper is launched; target never falls back.
    return spec;
#else
    (void)original;
    return fail(ErrorCode::Unsupported, "Native extension sandbox unavailable on this OS/build");
#endif
}
Result<void> enforceLinuxSandbox(const ProcessSandboxProfile& profile) {
#if defined(__linux__) && defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_add_rule) && defined(SYS_landlock_restrict_self) && defined(LANDLOCK_SCOPE_SIGNAL) && defined(LANDLOCK_ACCESS_FS_IOCTL_DEV)
    if (syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION) < 6)
        return fail(ErrorCode::Unsupported, "Landlock ABI 6 is unavailable");
    constexpr std::uint64_t allFs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE | LANDLOCK_ACCESS_FS_IOCTL_DEV;
    landlock_ruleset_attr rules{};
    rules.handled_access_fs = allFs;
    rules.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | (profile.network == SandboxNetwork::Deny ? LANDLOCK_ACCESS_NET_CONNECT_TCP : 0);
    rules.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
    const int rulesFd = static_cast<int>(syscall(SYS_landlock_create_ruleset, &rules, sizeof(rules), 0));
    if (rulesFd < 0) return fail(ErrorCode::Unsupported, "Cannot create complete native Landlock policy");
    struct Close { int fd; ~Close() { close(fd); } } closeRules{rulesFd};
    for (const auto& grant : profile.paths) {
        open_how how{}; how.flags = O_PATH | O_CLOEXEC; how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
        const int fd = static_cast<int>(syscall(SYS_openat2, AT_FDCWD, grant.path.c_str(), &how, sizeof(how)));
        if (fd < 0) return fail(ErrorCode::PermissionDenied, "Sandbox grant is unavailable or linked: " + grant.path);
        Close closePath{fd}; struct stat status{};
        if (fstat(fd, &status) != 0 || (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)) || (grant.recursive && !S_ISDIR(status.st_mode)))
            return fail(ErrorCode::Unsupported, "Sandbox grants require regular files or explicit directory subtrees");
        if (S_ISDIR(status.st_mode) && !grant.recursive)
            return fail(ErrorCode::Unsupported, "Landlock directory grants are recursive; exact directory grants are not representable");
        std::uint64_t allowed = 0;
        if (hasSandboxAccess(grant.access, SandboxAccess::Read)) allowed |= LANDLOCK_ACCESS_FS_READ_FILE | (S_ISDIR(status.st_mode) ? LANDLOCK_ACCESS_FS_READ_DIR : 0);
        if (hasSandboxAccess(grant.access, SandboxAccess::Execute)) allowed |= LANDLOCK_ACCESS_FS_EXECUTE;
        if (hasSandboxAccess(grant.access, SandboxAccess::Write)) allowed |= LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE |
            (S_ISDIR(status.st_mode) ? LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REFER : 0);
        landlock_path_beneath_attr path{}; path.parent_fd = fd; path.allowed_access = allowed;
        if (syscall(SYS_landlock_add_rule, rulesFd, LANDLOCK_RULE_PATH_BENEATH, &path, 0) != 0)
            return fail(ErrorCode::PermissionDenied, "Cannot install sandbox content grant: " + grant.path);
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || syscall(SYS_landlock_restrict_self, rulesFd, 0) != 0)
        return fail(ErrorCode::PermissionDenied, "Cannot enter native Landlock domain");
    std::vector<sock_filter> filter;
#if defined(__x86_64__)
    constexpr unsigned architecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr unsigned architecture = AUDIT_ARCH_AARCH64;
#else
    constexpr unsigned architecture = 0;
    return fail(ErrorCode::Unsupported, "Sandbox seccomp architecture is not supported");
#endif
    auto stmt = [&](unsigned short code, unsigned value) { filter.push_back(sock_filter{code, 0, 0, value}); };
    auto jump = [&](unsigned value, unsigned char yes, unsigned char no) { filter.push_back(sock_filter{BPF_JMP | BPF_JEQ | BPF_K, yes, no, value}); };
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)); jump(architecture, 1, 0); stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
#if defined(__x86_64__)
    filter.push_back(sock_filter{BPF_JMP | BPF_JSET | BPF_K, 0, 1, 0x40000000U}); stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
#endif
    auto deny = [&](unsigned nr) { jump(nr, 0, 1); stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM); };
    // No ambient host IPC, privilege, namespaces, new sessions, asynchronous syscall bypass or
    // cross-process memory/fd acquisition. Future syscalls are not claimed to be a full VM boundary.
    deny(SYS_socketpair); deny(SYS_bind); deny(SYS_listen); deny(SYS_accept); deny(SYS_accept4);
    deny(SYS_setsid); deny(SYS_setpgid); deny(SYS_unshare); deny(SYS_setns); deny(SYS_ptrace);
    deny(SYS_process_vm_readv); deny(SYS_process_vm_writev); deny(SYS_pidfd_getfd); deny(SYS_io_uring_setup);
    deny(SYS_bpf); deny(SYS_perf_event_open); deny(SYS_userfaultfd); deny(SYS_open_by_handle_at);
    deny(SYS_mount); deny(SYS_umount2); deny(SYS_pivot_root); deny(SYS_chroot);
    deny(SYS_keyctl); deny(SYS_add_key); deny(SYS_request_key);
    // clone3 flags live behind a pointer; force libc's clone fallback, whose flags are inspectable.
    jump(SYS_clone3, 0, 1); stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
    jump(SYS_clone, 0, 4); stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0]));
    filter.push_back(sock_filter{BPF_JMP | BPF_JSET | BPF_K, 0, 1, CLONE_NEWCGROUP | CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUSER | CLONE_NEWUTS});
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM); stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
    if (profile.network == SandboxNetwork::Deny) deny(SYS_socket);
    else {
        // Authorized outgoing Internet sockets only: no Unix/netlink/raw sockets or listeners.
        jump(SYS_socket, 0, 10);
        stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])); jump(2, 1, 0); jump(10, 0, 5);
        stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[1])); stmt(BPF_ALU | BPF_AND | BPF_K, 15);
        jump(1, 1, 0); jump(2, 0, 1); stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM); stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
    }
    stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) return fail(ErrorCode::Unsupported, "Cannot install native no-network/process seccomp policy");
    return {};
#else
    (void)profile;
    return fail(ErrorCode::Unsupported, "Build/kernel lacks complete Landlock ABI6 policy support");
#endif
}
} // namespace detail
} // namespace frost::io
