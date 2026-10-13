// The machine's file system, as the machine's half uses it: paths are the
// system's own (UTF-8; Windows' long ones too), symbolic links are never
// followed. What is received is written, and what is sent listed and read,
// through a Directory, so that it stays under the directory the host gave.
// Each platform has its own (files_<os>.cpp). Failures throw Error(DEVICE)
// with the system's reason, unless said otherwise. Their messages never
// hold a path of the machine: they go to the other end, and to logs a host
// may pass on; a Directory names what is in it by its path below the one
// the host gave (as items go), and that one as its opener says.
#pragma once

#include "common.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace rcsdk::file_transfer::files {

struct Status {
    rcs_file_kind kind = RCS_FILE_OTHER; // OTHER: a symbolic link, a junction, a device...
    uint64_t size = 0;                   // A file's.
    int64_t modified = 0;                // Nanoseconds since 1970 UTC.
};

class File;

// Whether `path` is a full path here (not relative to a working directory).
bool absolute(std::string_view path);
// `relative` (components separated by '/') under `directory`.
std::string join(std::string_view directory, std::string_view relative);
// The last component of a full path; empty for a root.
std::string lastName(std::string_view path);
// The directory a full path is in: the path without its last component.
std::string parentPath(std::string_view path);
// What is at `path` itself (a link is OTHER); nullopt when nothing is.
// Messages call it `shown`.
std::optional<Status> status(const std::string& path, const std::string& shown);
// The bytes this process may still write on the volume of `directory`, the
// receiving one (the test seam's answer, setFreeSpace, where it gives one)...
uint64_t freeSpace(const std::string& directory);
// ...and the system's own.
uint64_t systemFreeSpace(const std::string& directory);

// An open directory, where files are received. Everything done through it
// stays in it: names are single components, opened relative to it and never
// followed where they are links, and it stays the directory it was when it
// was opened, wherever its path leads later (another process that can
// write there may swap a directory on the way for a link). On Windows the
// directory and those above it, up to the one the host gave, are held open
// without FILE_SHARE_DELETE: they cannot be renamed or deleted meanwhile.
class Directory {
public:
    // The directory at a full path; links on the way are followed (the host
    // chose it: macOS' /tmp is one). Messages call it `shown`, and what is
    // in it by its name below `below` (the path of this directory as items
    // go; "": the names alone). Error(INVALID_ARGUMENT) when no directory is
    // there.
    static Directory open(const std::string& path, std::string shown, std::string below = {});

    // The directory `relative` ('/'-separated components, "" for this one)
    // under this one, a component at a time, never through a link: nullopt
    // where a component is not there, Error(INVALID_STATE) where one is a
    // link (a junction) or no directory.
    std::optional<Directory> find(std::string_view relative) const;
    // The same, made where a component is not there.
    Directory make(std::string_view relative) const;

    // The names in it, sorted (bytewise).
    std::vector<std::string> list() const;
    // The volume it is on: the same number for directories of one volume.
    uint64_t volume() const;
    // What is at `name` itself (a link is OTHER); nullopt when nothing is.
    std::optional<Status> status(const std::string& name) const;
    // A file to read: Error(INVALID_STATE) for a link or anything else that
    // is no file (a pipe is never waited on). Messages call it `shown`
    // ("": its name below this directory).
    File read(const std::string& name, const std::string& shown = {}) const;
    // A new file to write, made by this call: Error(INVALID_STATE) when
    // anything is at `name` already (a link too, never followed).
    File create(const std::string& name) const;
    // A file there to write from `offset` on (at 0 it is made where none
    // is): a file of its own (no other hard link to it) holding at least
    // `offset` bytes, cut to them. Error(INVALID_STATE) for anything else
    // there (a link, never followed; a pipe, never waited on) and for fewer
    // bytes.
    File write(const std::string& name, uint64_t offset) const;
    // Moves `from` to `to`, both in this directory; replace: over a file
    // there. Without replace, false when something is at `to`.
    bool rename(const std::string& from, const std::string& to, bool replace) const;
    // Deletes a file (or a link itself); nothing when none is there.
    void remove(const std::string& name) const;
    // Sets the modification time of what is at `name` itself (a directory's too).
    void setModified(const std::string& name, int64_t modified) const;

private:
    struct Handle; // The system's, and on Windows those of the directories above it.
    Directory(std::shared_ptr<const Handle> handle, std::string shown, std::string below)
        : handle_(std::move(handle)), shown_(std::move(shown)), below_(std::move(below)) {}
    std::optional<Directory> walk(std::string_view relative, bool make) const;
    std::optional<Directory> child(const std::string& name, bool make) const;
    // What messages call `name` in it.
    std::string under(std::string_view name) const;

    std::shared_ptr<const Handle> handle_;
    std::string shown_, below_; // For messages: this directory, and the path of what is in it.
};

// An open file.
class File {
public:
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File();

    Status status() const;
    // Exactly data.size() bytes from `offset`; Error(INVALID_STATE) where
    // the file ends sooner.
    void readAt(uint64_t offset, std::span<uint8_t> data);
    // Writes at the end of what was written (from the offset it was opened at).
    void append(std::span<const uint8_t> data);
    uint64_t position() const noexcept { return position_; }

private:
    friend class Directory;
    File(intptr_t handle, std::string path) : handle_(handle), path_(std::move(path)) {}
    void close() noexcept;

    intptr_t handle_ = -1; // A descriptor, or Windows' HANDLE.
    std::string path_;     // For messages, as its directory names it.
    uint64_t position_ = 0;
};

// The free space of every directory is `space`'s answer (for tests) where it
// gives one; empty restores the system's.
using FreeSpace = std::function<std::optional<uint64_t>(const std::string& directory)>;
void setFreeSpace(FreeSpace space);

} // namespace rcsdk::file_transfer::files
