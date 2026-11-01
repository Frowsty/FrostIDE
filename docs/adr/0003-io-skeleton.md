# ADR 0003: frost::io skeleton (paths, file system, processes)

- Status: accepted
- Date: 2026-10-31
- Plan reference: PLAN.md sections 3.2 (layers), 3.3 (errors), 3.5 (trust model: absolute tool paths),
  6.2 (deterministic doubles), 6.5 (wide Win32 APIs only); milestone M0

## Context
Everything above `core` that touches the disk or starts a tool goes through `frost::io`. M0 needs the
synchronous core of it: UTF-8/UTF-16 conversion, path handling, an `IFileSystem` with atomic writes, an
`IProcessLauncher` that is the only way to start child processes, and the deterministic doubles PLAN.md 6.2
names (MemFs with a virtual clock, scripted processes). The asio reactor, async pipes and file watchers
arrive in M1, supervised processes in M3 (the supervisor) and `IHttpClient` in M5 (Windows) and M6 (Linux).

Constraints that shaped the design:
- The reference Windows machine has `LongPathsEnabled = 0`, and users' machines often do too. Paths longer
  than MAX_PATH (260 UTF-16 units) are common in build trees (vcpkg, node_modules, nested CMake binary dirs).
- Child processes run user-controlled tools on user-controlled arguments; quoting bugs are injection bugs.
- Tests must not leave stray processes or temp files, and `ctest -j` and concurrent worktrees must not
  interfere with each other.
- No exceptions across modules; fallible APIs return `Result<T>`.

## Decision

### Strings and paths
- The API speaks UTF-8 `std::string`. Conversion to and from UTF-16 is strict: ill-formed UTF-8 (per Unicode
  table 3-7: overlongs, surrogates, > U+10FFFF, truncation) and unpaired surrogates fail with
  `InvalidArgument` naming the offset; nothing is replaced with U+FFFD. Directory entries whose UTF-16 name
  is not valid Unicode are skipped by `listDirectory` (TODO(M1): report them).
- Every path given to `IFileSystem` or `IProcessLauncher` must be absolute in the platform's syntax; relative
  paths fail with `InvalidArgument` and are never resolved against the current directory. Windows accepts
  drive (`C:\`), UNC (`\\server\share\`) and verbatim (`\\?\`) roots; drive-relative (`C:x`), rooted (`\x`)
  and incomplete UNC forms are rejected. Device-namespace paths (`\\.\`) are rejected too: the OS
  normalises them (`\\.\C:\d\t.bat\.` opens `C:\d\t.bat`), so no check on the string would see the file
  actually opened.
- The invariant behind the path rules: the string frost::io checks is the file the OS opens. Windows
  normalisation turns `/` into `\`, collapses repeated separators, removes `.` and resolves `..` lexically
  (never above the root), as `GetFullPathNameW` does. Components that Win32 would rewrite are refused in
  drive and UNC paths rather than created literally under `\\?\`: names ending in a dot or a space (Win32
  strips them) and reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM0-9`, `LPT0-9` and the superscript
  forms, with or without an extension). An explicit `\\?\` path is the escape hatch for such names; it is
  passed literally but may not contain empty, `.` or `..` components, nor `/`. Every Windows form refuses
  `< > : " | ? *` and control characters in names: they are invalid or wildcards in Win32 names, and `:`
  would select an alternate data stream (`t.bat::$DATA` is `t.bat`), which no name check could follow.
  POSIX normalisation keeps `..` (symlinks make it non-lexical); the kernel resolves it against the real tree.
- On Windows every file-system call uses the extended-length form (`\\?\C:\...`, `\\?\UNC\server\share\...`),
  so MAX_PATH never applies to frost::io, whatever `LongPathsEnabled` says. Only wide (`...W`) APIs are used.

### Errors
`os_error.hpp` holds one documented mapping from errno and Win32 codes to `frost::ErrorCode` (for example
`ENOENT`/`ENOTDIR`/`ERROR_PATH_NOT_FOUND` → NotFound, `EACCES`/`ERROR_ACCESS_DENIED` → PermissionDenied,
`ERROR_SHARING_VIOLATION`/`EBUSY` → Busy). Messages read `"<operation> '<path>': <system text> (win32 error N)"`
or `(errno N)`, so logs keep the raw code. Where the OS answer is ambiguous the implementations refine it
the same way on both platforms ("is a directory" / "is not a directory" → InvalidArgument; creating a
directory where a file is → AlreadyExists).

### IFileSystem
- Synchronous and blocking (callers run it on scheduler threads; TODO(M1): async variants on the reactor).
  Operations: `readFile`, `writeFileAtomic`, `status` (type, size, mtime), `listDirectory` (sorted by name),
  `createDirectories`, `removeFile`, `removeDirectory` (empty only), `exists`.
- `writeFileAtomic` writes a temp file `.<name>.frost-tmp-<pid>-<n>` in the target's directory (created
  exclusively), flushes it (`FlushFileBuffers` / `fsync`), then replaces the target atomically:
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows, `rename` plus a best-effort
  `fsync` of the directory on POSIX. On any failure the temp file is removed and the old content is intact.
  POSIX metadata: a new file is created with `open(O_CREAT, 0666)` and no `fchmod`, so the kernel applies
  the umask or, where the directory has one, its default ACL, as for a file any other program creates
  there. A replaced file's temp file is created `0600` and, once its content is written, loses any access
  ACL it inherited from the directory's default ACL (its named entries would otherwise read the target's
  group bits as their mask), receives the target's owner and group (`fchown`, which needs CAP_CHOWN for
  the owner and membership for the group), then its permission bits (`fchmod`, after `fchown` because
  that clears set-id bits), and last the target's access ACL (`system.posix_acl_access`, after `fchmod`
  because that rewrites the ACL mask) when owner and group were both kept. What cannot be carried over
  fails safe, so no one gets more access than the target gave them (`replacementMode` in
  `src/io/src/internal.hpp`, unit-tested on every platform): a different group drops S_ISGID and gives the
  group and other classes only what both had (0664 becomes 0644, 0640 becomes 0600), since old members
  become others and others may become members; a different owner drops S_ISUID and limits both classes to
  the owner's bits; an ACL that is not copied, or whose copy fails, leaves both classes only what every
  non-owner entry granted, because named entries may deny. Other extended attributes are not carried
  over. A read-only target is replaced on POSIX (rename needs write access to the directory only) and
  keeps its mode; Windows refuses it with PermissionDenied. Windows does not carry over attributes, ACLs,
  alternate streams or hard links (the SaveManager owns that policy in M1). TODO(M1): sweep stale temp
  files left by crashes.
- `readFile` reads regular files only: FIFOs, sockets and devices fail with InvalidArgument without
  blocking (POSIX opens with `O_NONBLOCK | O_NOCTTY` and checks `fstat` before reading; Windows checks
  `GetFileType`). Sockets, which refuse `open` with ENXIO, are reported the same way rather than as
  NotFound. A regular file under another process's write lease (Samba oplocks, NFS delegations) makes the
  non-blocking open fail with EWOULDBLOCK after starting the lease break; `readFile` retries with backoff
  (still non-blocking, so a FIFO swapped in cannot hang it) until the holder releases the lease or the
  kernel revokes it (`lease-break-time`, 45 s by default), and gives up with Busy after 90 s.
- Links: `listDirectory` reports symlinks and Windows junctions as `Symlink`; `status` follows them.
  `removeFile` removes such an entry itself on both platforms, never its target (Windows uses
  `RemoveDirectoryW` for directory links, which deletes only the reparse point), and `removeDirectory`
  refuses a link with InvalidArgument, as `rmdir` does on POSIX. So a recursive delete that removes
  `Symlink` entries with `removeFile` never descends into, or empties, a link's target.
- `realFileSystem()` is a stateless, thread-safe, `constinit` singleton.
- `MemFs` implements the same contract in memory with mtimes from an `IClock`, either path style on any
  platform, and fault injection per operation, path and count (including the rename step of an atomic write,
  so the "old content survives" path is testable). It refuses what the real implementations refuse: the
  Windows name rules above, POSIX `..` through a missing directory or a file (resolved against the tree, as
  the kernel does), a final `..` as an entry to create or remove, and roots that do not exist (only `/` or
  `C:\` exists until `addRoot`). `createDirectories` follows the real walk-up-then-create algorithm, so
  `/w/new/../out` creates both `new` and `out` as `mkdir -p` does. Windows-style verbatim spellings name the
  plain entries (`\\?\C:\x` is `C:\x`, `\\?\UNC\s\sh\x` is `\\s\sh\x`). A fault fires for an operation
  whose own spelling or whose resolved entry matches the fault's path. It is case-sensitive apart from
  drive letters, even in Windows style (TODO(M2): case-insensitive mode for Windows-style trees).

### IProcessLauncher
- `ProcessSpec`: absolute executable, UTF-8 arguments, optional absolute working directory, a clean or
  inherited environment plus ordered overrides (value sets, `nullopt` removes; names case-insensitive on
  Windows), and per-stream `Inherit`/`Null`/`Pipe` (plus `MergeIntoStdout` for stderr). Every launcher
  validates specs with the same `validateProcessSpec`, including the Windows-only checks that
  `CreateProcessW` would otherwise fail later (no quote in the executable, a command line under 32,767
  UTF-16 units, well-formed UTF-8 in the command line, working directory and environment), so the
  scripted double refuses every spec the real launcher refuses before touching the system.
- `Inherit` never hands a child an invalid stream: a parent without such a standard handle (a GUI parent,
  a closed descriptor) gives the child the null device. On POSIX a descriptor marked close-on-exec counts
  as absent too: once a process closes, say, fd 0, the next descriptor frost::io opens (a pidfd, a pipe
  end of a concurrent launch) lands there, vanishes at exec, and would leave the child's slot empty for
  its own next `open` to take.
- `IProcess` exposes `pid`, blocking `writeStdin`/`closeStdin`/`readStdout`/`readStderr`, `wait(timeout)`,
  `terminate`, and `communicate(stdin, timeout)`, which feeds stdin and drains both outputs concurrently on
  one thread, bounded by the deadline; on timeout it terminates and reaps the process and returns Timeout.
  `runToCompletion` is launch + communicate. `communicate` is a virtual on `IProcess` so the scripted double
  implements it without real time.
- Windows: `CreateProcessW` with an explicit application name (plain below MAX_PATH, `\\?\` beyond), a
  command line built by the exact inverse of the MSVC CRT / `CommandLineToArgvW` rules (cited in
  `process.hpp`), `STARTUPINFOEXW` with `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (only the three std handles are
  inherited) and `PROC_THREAD_ATTRIBUTE_JOB_LIST` (the child starts inside a kill-on-close Job Object when
  `killOnClose`), `CREATE_UNICODE_ENVIRONMENT`, and `CREATE_NO_WINDOW` only when the parent has no console.
  Pipes are overlapped, local-only (`PIPE_REJECT_REMOTE_CLIENTS`), single-instance named pipes, so
  `communicate` can wait on all of them with `WaitForMultipleObjects`. Their names carry 128 random bits
  from `BCryptGenRandom` and their DACL grants access to the current user's SID only, so another process
  can neither predict a name nor connect first. `.bat`/`.cmd` executables are refused with Unsupported
  because `cmd.exe` re-parses the command line with different rules ("BatBadBut"). The check runs on the
  given spelling, on the normalised path the launcher starts, and, just before `CreateProcessW`, on
  `GetFullPathNameW` of the application name, so trailing separators, dots, spaces or `x\..` cannot hide
  the extension. TODO(M2): a dedicated cmd launcher.
- Linux: `posix_spawn` (never `posix_spawnp`) with `pipe2(O_CLOEXEC)` pipes, `addclosefrom_np(3)` and
  `addchdir_np` (glibc ≥ 2.34; Ubuntu 24.04 ships 2.39), default signal mask and dispositions, and, when
  `killOnClose`, a new session (`POSIX_SPAWN_SETSID`, glibc ≥ 2.26). The session's process group has the
  child's pid as its id, so `kill(-pid)` ends every process that stays in the group (a descendant that
  calls `setsid` or `setpgid`, as daemons do, leaves it; on Windows the job holds the whole tree, since
  descendants cannot break away), and it has no controlling terminal: a tool that
  opens `/dev/tty` for a prompt gets ENXIO and fails, where in a mere new process group it would be a
  background job of the IDE's terminal and be stopped by SIGTTIN while `wait` blocks. Being outside the
  terminal's job control also means such a child could read an inherited terminal stdin while another job
  is in the foreground (the shell, after `frost &`), splitting the user's keystrokes, a typed password
  included, and change the terminal's modes without SIGTTOU. A contained child's inherited terminal stdin is
  therefore replaced by `/dev/null`; terminal stdout and stderr are kept, and without `killOnClose` the
  child shares the IDE's job control and gets the terminal. A launch-time check of the foreground
  process group (`tcgetpgrp`) would not do: the user can background the IDE later, and the session child
  would never notice. `wait` uses `waitid(WNOWAIT)` (with a pidfd where available) so the
  zombie is kept until the `IProcess` is destroyed and a later `kill` can never hit a recycled pid, as
  long as nothing else in the process reaps the child (no SIGCHLD `SIG_IGN` or `SA_NOCLDWAIT`, no
  `waitpid(-1)`; the M1 child watcher must reap detached pids only). Writes
  to a child that closed stdin block SIGPIPE for the calling thread and consume it, so a broken pipe is an
  error value, never a signal.
- `terminate()` reports exit code `kTerminatedExitCode` (137 = 128 + SIGKILL) on both platforms; POSIX also
  reports the signal.
- Deadlines saturate: `timeout` values up to `Duration::max()` mean "effectively no deadline" instead of
  overflowing `steady_clock::now() + timeout`. A single `poll` or `Wait*` call that has a deadline blocks
  for at most 2^30 ms (about 12.4 days), and every such wait re-checks its deadline when a slice ends, so
  a far deadline is never cut short; tests lower the slice to cover that loop. Waits without a deadline
  (`wait()` with no timeout, and the blocking `readStdout`, `readStderr` and `writeStdin`) still block in
  one call: `WaitForSingleObject(INFINITE)` or `GetOverlappedResult(..., TRUE)` on Windows, a blocking
  `waitid` or `poll(-1)` on POSIX.
- Not yet: logging of the resolved launch (PLAN.md 3.5; TODO(M1) once core has logging), async pipes on the
  reactor (TODO(M1)), supervision with restart (TODO(M3), the M3 supervisor).

### Doubles
`MemFs` and `ScriptedProcessLauncher` live in `frost_io` itself, next to the interfaces they implement, so
every module's tests can use them without a separate library. `frost_io` is static and each double is its
own object file, so they are linked only into binaries that reference them. `ScriptedProcessLauncher` queues
expected launches (FIFO, optionally pinned to an executable), records every launch with its stdin, models
launch failures and hung children (`neverExits`), and applies the real launcher's validation.

## Measurements
Debug build (clang-cl, MSVC STL 14.44), Windows 11 x64 reference machine, per-test gtest timings from
three consecutive runs of `io_integration_tests` with no build running (ranges are min-max):

| Scenario | Time |
|---|---|
| `communicate`: child writes 3 MiB to stdout and 3 MiB to stderr, interleaved in 4 KiB chunks | 53-62 ms |
| `communicate`: 2 MiB fed to stdin, echoed to both stdout and stderr | 36-192 ms |
| Launch, exit and reap, 5 children in sequence | 93-412 ms (19-82 ms per child) |
| `communicate` with a 300 ms deadline on a child that sleeps 60 s: Timeout returned after | 315-468 ms |
| `writeFileAtomic` + `readFile` of 200 KB | 4-5 ms |
| Tree beyond MAX_PATH: create, write, read, status, list, replace, remove | 9-11 ms |

The launch variance comes from on-access antivirus scanning of the freshly built child executable. In WSL 2
on the same machine (Linux Debug) the 38 integration tests of `io` and `app` take 0.73 s in total. No
deadlock or timeout was seen in 30 repeated ctest runs of the handle-inheritance test or in the full suites.

## Consequences
- Long paths work in frost::io on every Windows machine. A child's *current directory* beyond MAX_PATH is
  still refused by Windows when `LongPathsEnabled = 0`; the launcher reports that as InvalidArgument with an
  explanation (TODO(M1): fall back to the 8.3 short name). Qt cannot load a plugin whose path is longer
  than 250 characters on such machines (measured with Qt 6.12.0; `kQtPluginPathLimitUnits` in
  `src/app/include/frost/app/startup.hpp`), so frost.exe itself cannot start from a directory beyond
  MAX_PATH there, unless it is started through the directory's 8.3 short name, which Qt then uses as
  well. Its start-up check reports that instead of letting Qt abort. In
  `tests/app/frost_process_test.cpp`, `RunsFromPathLongerThanMaxPath` skips there, so the M0 exit
  criterion stays visibly unmet, and `ReportsLongPathLimitCleanly` covers the report.
- Process-wide hardening that PLAN.md 3.5 lists next to the launcher (`SetDefaultDllDirectories`,
  `NoDefaultCurrentDirectoryInExePath`) belongs to app startup (`src/app/include/frost/app/startup.hpp`),
  not to frost::io. As a consequence, DLLs are no longer found through PATH, and
  `LoadLibraryExW(bare name, LOAD_WITH_ALTERED_SEARCH_PATH)` fails with ERROR_INVALID_PARAMETER;
  third-party code loaded into frost that relies on either (IMEs, shell extensions) breaks, as in
  Chromium and Firefox. `SearchPathW` still searches the current directory, so frost-lint bans it.
- Callers must use `communicate`/`runToCompletion` unless they deliberately service one pipe at a time;
  the blocking per-stream calls can deadlock if misused (documented on `IProcess`).
- Tests use `tests/support/scratch_dir.hpp`: one unique directory per test under the system temp directory,
  removed through `realFileSystem()`, so concurrent `ctest -j` runs and worktrees never share files.

## Alternatives considered
- `ReplaceFileW` for atomic writes: preserves attributes and ACLs, but without a backup file it can fail
  with `ERROR_UNABLE_TO_MOVE_REPLACEMENT` after the target has been moved away, leaving no file at the
  target path. `MoveFileExW` either replaces or leaves the old file; metadata preservation moves to the
  SaveManager.
- Anonymous pipes (`CreatePipe`) on Windows: they cannot be used with overlapped I/O, so draining stdout
  and stderr together needs one thread per pipe. Named pipes with `FILE_FLAG_OVERLAPPED` keep
  `communicate` single-threaded and deadline-bounded (the approach Rust's standard library takes, MIT/Apache).
- `bInheritHandles = TRUE` without a handle list: leaks every inheritable handle any library opened into
  every child (and keeps pipes open, breaking EOF). The explicit list is the documented fix.
- `fork` + `exec` on Linux: needs async-signal-safe code between fork and exec and is slow for large
  parents; `posix_spawn` with the glibc 2.34 file actions covers every need (chdir, closefrom).
- Quoting batch files for `cmd.exe`: possible only with cmd-specific escaping that depends on the batch
  file's own use of its arguments; refusing them now is safer than a partial escape.
- Lossy UTF-8 conversion (replace with U+FFFD): silently changes file names, so a file read under one name
  could be written back under another. Strict conversion with an error is the only safe default.
- Doubles in a separate test-only library: adds a target per module for no benefit while they carry no
  security-relevant behaviour (contrast the approval double of PLAN.md 5, which must stay test-only).
