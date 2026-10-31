# ADR 0003: frost::io skeleton (paths, file system, processes)

- Status: accepted
- Date: 2026-10-31
- Plan reference: PLAN.md sections 3.2 (layers), 3.3 (errors), 3.5 (trust model: absolute tool paths),
  6.2 (deterministic doubles), 6.5 (wide Win32 APIs only); milestone M0

## Context
Everything above `core` that touches the disk or starts a tool goes through `frost::io`. M0 needs the
synchronous core of it: UTF-8/UTF-16 conversion, path handling, an `IFileSystem` with atomic writes, an
`IProcessLauncher` that is the only way to start child processes, and the deterministic doubles PLAN.md 6.2
names (MemFs with a virtual clock, scripted processes). The asio reactor, async pipes, file watchers,
`IHttpClient` and supervised processes arrive in M1 and later.

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
  drive (`C:\`), UNC (`\\server\share\`), verbatim (`\\?\`) and device (`\\.\`) roots; drive-relative
  (`C:x`), rooted (`\x`) and incomplete UNC forms are rejected.
- Windows normalisation turns `/` into `\`, collapses repeated separators, removes `.` and resolves `..`
  lexically (never above the root), and keeps trailing dots and spaces (they are significant under `\\?\`).
  Verbatim and device paths are passed through untouched. POSIX normalisation keeps `..` (symlinks make it
  non-lexical).
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
  POSIX keeps the target's permission bits; Windows does not carry over attributes, ACLs, alternate streams
  or hard links (the SaveManager owns that policy in M1). TODO(M1): sweep stale temp files left by crashes.
- `realFileSystem()` is a stateless, thread-safe, `constinit` singleton.
- `MemFs` implements the same contract in memory with mtimes from an `IClock`, either path style on any
  platform, and fault injection per operation, path and count (including the rename step of an atomic write,
  so the "old content survives" path is testable). It is case-sensitive even in Windows style
  (TODO(M2): case-insensitive mode for Windows-style trees).

### IProcessLauncher
- `ProcessSpec`: absolute executable, UTF-8 arguments, optional absolute working directory, a clean or
  inherited environment plus ordered overrides (value sets, `nullopt` removes; names case-insensitive on
  Windows), and per-stream `Inherit`/`Null`/`Pipe` (plus `MergeIntoStdout` for stderr). Every launcher
  validates specs with the same `validateProcessSpec`.
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
  Pipes are uniquely named, overlapped, local-only named pipes, so `communicate` can wait on all of them
  with `WaitForMultipleObjects`. `.bat`/`.cmd` executables are refused with Unsupported because `cmd.exe`
  re-parses the command line with different rules ("BatBadBut"); TODO(M2): a dedicated cmd launcher.
- Linux: `posix_spawn` (never `posix_spawnp`) with `pipe2(O_CLOEXEC)` pipes, `addclosefrom_np(3)` and
  `addchdir_np` (glibc ≥ 2.34; Ubuntu 24.04 ships 2.39), default signal mask and dispositions, and a new
  process group when `killOnClose`. `wait` uses `waitid(WNOWAIT)` (with a pidfd where available) so the
  zombie is kept until the `IProcess` is destroyed and a later `kill` can never hit a recycled pid. Writes
  to a child that closed stdin block SIGPIPE for the calling thread and consume it, so a broken pipe is an
  error value, never a signal.
- `terminate()` reports exit code `kTerminatedExitCode` (137 = 128 + SIGKILL) on both platforms; POSIX also
  reports the signal.
- Not yet: logging of the resolved launch (PLAN.md 3.5; TODO(M1) once core has logging), async pipes on the
  reactor (TODO(M1)), supervision with restart (TODO(M1)).

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
  explanation (TODO(M1): fall back to the 8.3 short name). Qt loads its plugins through plain Win32 paths,
  so frost.exe itself cannot start from a directory beyond MAX_PATH on such machines (the app test skips
  there; see `tests/app/frost_process_test.cpp`).
- Process-wide hardening that PLAN.md 3.5 lists next to the launcher (`SetDefaultDllDirectories`,
  `NoDefaultCurrentDirectoryInExePath`) belongs to app startup, not to frost::io.
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
