# Opt-in native extension content sandbox (UX20 source checkpoint)

Status: authored source and deferred regression fixtures; no launch, policy installation,
AppContainer creation, host ACL/privilege change, configuration, build or test has been executed.
The unchanged default ProcessSpec is ordinary process-tree containment, not a content sandbox.

An explicit ProcessSandboxProfile names absolute content paths, Read/Write/Execute rights and
exact-file or recursive-directory scope. Network is denied unless the caller explicitly selects
InternetClient. Profiles require cleanEnvironment, pipe/null stdio and killOnClose; inherited extra
FDs, terminals, raw shell argument strings, desktops and detached processes are refused. Both
production sync and async launch routes enter the same native spawn boundary. Unsupported setup
never retries the extension as an ordinary process.

## Windows

The extension owner supplies captured, hash-approved explicit SandboxStagingFiles including all
nonambient runtime dependencies. stageWindowsSandboxProcess creates a new random private
AppContainer identity and a new protected staging generation under an existing private parent.
Only newly created private files/directories receive ACLs; original tools, user files and workspace
ACLs are never changed. Each file receives its approved rights. Staged directory name-listing and
traversal are available so runtimes can locate their captured files. Writable staged files receive
a low integrity label. Exact executable and argument path matches are rewritten to their staged
identities; no substring rewriting or ambient runtime directory copying occurs.

CreateProcess receives SECURITY_CAPABILITIES plus the LPAC all-application-packages opt-out
attribute at creation, alongside the existing kill-on-close job/explicit inherited standard handles.
Default capabilities are empty. Only explicit InternetClient adds that Windows capability. Missing
LPAC support, linked/unavailable staged paths, private profile creation or startup attribute failure
returns an error. Local drive paths only are supported; UNC/device staging is refused. Path handles
pin every staging ancestor through process creation with a bounded handle budget.

This is not a global exact Windows filesystem allowlist. Windows defines LPAC ambient system
access, and its own private AppContainer profile storage is an OS resource outside the staged
payload. The sample's user/project content is brokered through the existing stdio didOpen and
reviewed WorkspaceEdit boundary. No workspace read/write capability or original-path ACL grant
is added. Staging generations and their OS profiles are retained pending a separate safe collection
policy; setup failures can leave a new private partial generation/profile for explicit later cleanup.
The independent future fixture cleans up only its own random profile after its child has settled.

The clean child environment supplies the owned staging path as the `LOCALAPPDATA` input required
by process creation. Windows rewrites it to the child's private AppContainer profile location.

## Linux

The launcher requires enabled Landlock ABI 6 or newer and an owned frost-extension-sandbox helper
beside the actual calling executable. The helper opens explicit grants with openat2 no-symlink and
no-magic-link resolution, permits only regular files or explicitly recursive directories, sets
no_new_privs, enters a deny-by-default Landlock ruleset and installs seccomp before execve of the
extension. Exact directory grants are refused because Landlock directory rules apply recursively.
The caller must explicitly grant the executable, ELF loader and runtime files/subtrees it needs;
no /usr, /lib, home or workspace scope is added automatically.

For a Linux release build that provides this sandbox, select a reviewed public
`linux/landlock.h` exposing ABI 6 or newer and supply its independently verified SHA-256.
Ubuntu 24.04's default userspace header can lag the WSL kernel. Use the public UAPI
header from the appropriate kernel headers package when the default is too old:

```sh
cmake --preset linux-clang -DFROST_LANDLOCK_UAPI_HEADER=/absolute/linux/landlock.h \
  -DFROST_LANDLOCK_UAPI_SHA256=<expected-sha256>
cmake --build --preset linux-clang-release --target frost-extension-sandbox
```

The build uses only this single selected header for the Linux sandbox translation unit.
An explicit selection fails the build if complete ABI 6 enforcement cannot be compiled.
Validate the resulting helper on the release host using the real native sandbox fixture.

Known filesystem operations through ABI 6, cross-domain signals and abstract Unix connections
are restricted. Default seccomp denies socket creation; authorized InternetClient allows only
Internet stream/datagram sockets and still refuses listener and ambient Unix/netlink/socketpair
creation. Namespace/session escapes, cross-process memory/fd acquisition, io_uring, ptrace and
related kernel interfaces are refused. clone3 returns ENOSYS for libc's inspectable clone fallback;
namespace flags on clone are denied. Restrictions are inherited by descendants. Helper-loader
injection environment variables are refused before starting the helper.

If required headers/architecture/kernel enforcement are absent, setup refuses. Helper-side setup
or target exec failure exits 125 with FROST_SANDBOX_UNAVAILABLE on stderr, before extension
instructions; the product must surface this observed startup failure. It is not a successful
sandbox acceptance. Landlock does not hide metadata such as stat information and does not cover
all special kernel filesystems. The supplied protocol pipes intentionally remain inherited. This
profile is not a VM, a hostile-kernel boundary or a defense against a malicious same-user host
changing the approved installation/state. No claim is made for future syscall families or a
resource/CPU/memory quota beyond existing independent process controls.

## Deferred evidence

Pure production-policy tests cover exact rights, component boundaries, parent traversal, invalid
profiles and mandatory launch restrictions. The independent sandbox_probe_child uses only native
APIs/standard library to attempt permitted reading, forbidden reading and writing; Linux additionally
attempts socket creation, while Windows independently inspects AppContainer/capability token state.
The production integration fixture exercises both real synchronous and asynchronous launch paths.
It is explicitly opt-in with FROST_RUN_NATIVE_SANDBOX_PROBE; Linux also requires the exact canonical
runtime file inventory in FROST_SANDBOX_TEST_RUNTIME_FILES. Unset acceptance inputs are reported
as skipped rather than counted as native enforcement evidence. No fixture has been executed.

The implementation uses only existing platform headers/libraries and project code. No new
third-party source or package/license graph was introduced. Public primary references:
[Microsoft LPAC launch contract](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer),
[Linux Landlock API and limitations](https://www.kernel.org/doc/html/latest/userspace-api/landlock.html),
[Linux seccomp filter API](https://www.kernel.org/doc/html/latest/userspace-api/seccomp_filter.html).

Helper deployment is an always-run prerequisite of each application/test target, ordered after
the helper target. A helper-only recompilation is copied on the next caller-target build even
when the caller needs no relink; duplicate registrations share the same deployment target.
Source build-graph regressions cover those dependency edges and the app/native-probe consumers.
These checks remain authored and unexecuted; no incremental-build acceptance is claimed.
