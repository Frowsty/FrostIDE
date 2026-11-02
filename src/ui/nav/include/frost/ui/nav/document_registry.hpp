// SPDX-License-Identifier: Apache-2.0
//
// DocumentRegistry: the shell's map from paths to open documents (ADR 0042), the seam the W1
// workbench binds editor views to. Every open entry point of the shell (File System view, Go to File,
// Ctrl+E, session restore, the command line) goes through it: the path is canonicalized
// (IFileSystem::canonicalPath, so "Foo.cpp" and "foo.cpp" are one document on Windows) and opened
// through the SaveManager, which reads it through the LoadService with the large-file guard.
/*
 * Views. An editor view acquire()s its document's path and release()s it when it closes; the last
 * release of a clean document closes it in the SaveManager (a dirty one stays, journaled for hot
 * exit, until it is saved or reverted). Listeners hear about state changes the shell reports with
 * notifyChanged() (external changes, saves, merges), so banners and status follow.
 *
 * Thread safety: none; the UI thread, which owns the SaveManager (its document actor in M1).
 * Lifetime: the SaveManager and the file system outlive the registry.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/document/file_document.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/io/file_system.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui::nav {

struct OpenedDocument {
    document::FileDocument* document = nullptr;
    std::string path; // Canonical.
};

class DocumentRegistry {
public:
    using Listener = std::function<void(const std::string& path)>;

    DocumentRegistry(document::SaveManager& manager, io::IFileSystem& fs) noexcept;
    DocumentRegistry(const DocumentRegistry&) = delete ("views hold paths into the registry");
    DocumentRegistry& operator=(const DocumentRegistry&) = delete ("views hold paths into the registry");

    // The canonical spelling of `path`: IFileSystem::canonicalPath, or, for a path that does not exist
    // (yet), its lexical normal form; `path` itself when it is not absolute. O(path) plus one open.
    [[nodiscard]] std::string canonicalize(std::string_view path) const;
    /*
     * Opens (or finds) the document at `path` after canonicalize(). Errors: SaveManager::open's:
     * OutOfRange from the large-file guard ("has N bytes; files of 128 MiB or more are not opened",
     * decided from the file's status before any read), NotFound, PermissionDenied, IoError,
     * InvalidArgument (a relative path).
     */
    [[nodiscard]] Result<OpenedDocument> open(std::string_view path);
    // The open document at `path` (canonicalized), or nullptr.
    [[nodiscard]] document::FileDocument* find(std::string_view path) const;

    // View bookkeeping (see the header comment); `path` is canonical. release() of an unknown path
    // does nothing. O(log n).
    void acquire(const std::string& path);
    void release(const std::string& path);
    [[nodiscard]] int viewCount(std::string_view path) const;

    // Registers a listener for notifyChanged(); returns its id for unsubscribe().
    std::uint64_t subscribe(Listener listener);
    void unsubscribe(std::uint64_t id);
    // Tells every listener that the document at `path` (canonical) changed state.
    void notifyChanged(const std::string& path);

    [[nodiscard]] document::SaveManager& manager() const noexcept { return *m_manager; }
    [[nodiscard]] io::IFileSystem& fileSystem() const noexcept { return *m_fs; }

private:
    document::SaveManager* m_manager;
    io::IFileSystem* m_fs;
    std::map<std::string, int, std::less<>> m_views;
    std::vector<std::pair<std::uint64_t, Listener>> m_listeners;
    std::uint64_t m_nextListener = 1;
};

} // namespace frost::ui::nav
