# ADR 0060: FrostEngine external-editor open requests

Status: proposed, 2026-11-02.

FrostIDE accepts `--project <root-or-frostproj> <file>:<line>:<column>` as the engine's
script-editor contract specifies, and also supports separate `--line` and `--column` options.
Coordinates are 1-based; zero leaves that coordinate at the editor's current position. Explicit
project roots choose their .frostproj when present, bypass sibling solution discovery, and are
opened before the script. Multiple .frostproj files are refused as ambiguous. Plain startup still
uses the existing session and solution-discovery behavior.

A canonical project directory is SHA-256 keyed for one local owner; a symlinked .frostproj is
resolved before selecting its containing directory. Ordinary project opens and later project switches
use the same physical root identity. Existing editor paths are compared by physical file identity
before opening an external request, preserving a dirty tab originally opened through a symlink.
Before the first frame, an
owner reserves the name without a listener; the listener starts after the frame. A second process
waits for a bounded response and forwards the request instead of creating another window. The
GUI validates the request's project against its current root, resolves the requested file before
checking its type, and opens only editor documents. Project and solution files are refused even
through symlinks; other tabs and unsaved buffers stay in place. An owner that switches projects retires its old
server off the GUI thread and claims the new root. Failed forwarding is reported; it does not
silently create a duplicate window.

The Qt-free `frost::io` transport uses an ACL restricted to the current user and a named pipe on
Windows, with the user SID in both the pipe and global mutex names and server-token verification.
The global mutex's DACL grants only the current SID; Microsoft documents the global-object privilege
requirement for file mappings and symbolic links, not mutexes ([kernel object namespaces](https://learn.microsoft.com/en-us/windows/win32/termserv/kernel-object-namespaces)).
An unelevated-token test verifies the global mutex and matching pipe; actual multi-logon-session
behavior has not been exercised on this machine. Linux uses AF_UNIX in a
0700, owner-checked per-user directory, peer-credential checks and a per-project flock; a stale
socket is removed only while holding that lock. Both cap request and reply payloads at 64 KiB (the reply's status byte is additional), return an
error for oversized replies, enforce connection and reply deadlines, reject remote/wrong-user clients, and release reservations when processes exit.
This uses the existing native IPC policy rather than adding the banned Qt Network module. Qt is
used only in the app-layer adapter and GUI dispatch.

FrostEngine automation discovery, play control, logging, tests and script validation are separate
features, not part of this opening contract. Engine files and user settings remain unchanged.
