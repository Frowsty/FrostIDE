# ADR 0105: Bounded read-only large-file preview

- Status: accepted implementation profile; runtime acceptance pending
- Date: 2026-11-14

## Context

UX-8 replaces the shell refusal at 128 MiB with a useful first screen. Editable document loading,
reload, merge and hot-exit guards remain authoritative. No UI operation may build a gigabyte string
or a file-wide line table. B15 and the full large-file exit criteria remain unchanged.

## Decision

The document editor factory routes new regular files at or above the existing threshold to a
separate read-only editor view before registry admission. It never creates a placeholder editable
document. Existing documents, including journalled dirty documents, continue through their existing
load/reload/merge rules.

IFileSystem supplies a lifetime-independent opener copied without I/O. The worker opens a retained
native regular-file handle and reads at most 64 KiB per page using 4 KiB positioned chunks. Windows
uses wide paths and read-only sharing; Linux uses pread and before/after device, inode, size, mtime
and ctime checks. Every page rechecks the named path against the retained identity. Mutation or
replacement refuses the page; reopening establishes a new identity. The reader is owned by the worker
and closes after cancellation without a GUI join. Jobs retain no filesystem, view, host or widget
pointer. Stop tokens and generations suppress obsolete delivery.

The view offers previous/next byte pages, a 64-bit byte offset and cancellable line seeking. Line
seeking scans LF/CRLF on a worker in bounded chunks without retaining a global index; lone CR line
navigation is unsupported and the profile is labelled. UTF-8 text only is supported; binary/NUL,
UTF-16 BOM and malformed complete UTF-8 sequences are refused. Incomplete UTF-8 code points at byte
page edges may display replacement glyphs, explicitly labelled. A byte page may start mid-line,
so its global line is explicitly unknown until line seeking. Existing line-based sessions restore
a known line or the first page; arbitrary byte offsets are not persisted by this profile. There is no editing, syntax analysis,
whole-file search/index or large-file editing opt-in in this profile.

## Verification state

Native tests author an actual greater-than-1-GiB fixture with high-offset reads and retained identity
checks; a native 128-MiB UI fixture drives the product factory and checks first-screen contents with
no editable document. Deferred UI tests cover bounded line/byte seeking, generation cancellation,
close and filesystem lifetime, encoding constraints and identity errors. No fixture has been created
or test/build/benchmark executed during this implementation phase. This profile establishes no B15
performance result or 1-GiB acceptance claim.
