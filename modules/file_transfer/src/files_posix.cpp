// The file system on macOS, Android (and other POSIX systems). A Directory is a
// descriptor: everything under it goes through the *at() calls, each
// component opened with O_NOFOLLOW (a link there is never followed).
#include "files.hpp"

#include <cerrno>
#include <cstdio>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifndef __APPLE__
#include <sys/syscall.h>
#endif

namespace rcsdk::file_transfer::files {
namespace {

[[noreturn]] void failed(const std::string& what, const std::string& path, int code = errno) {
    fail(RCS_ERROR_DEVICE, what + " " + path + ": " + std::strerror(code));
}

int64_t nanoseconds(const struct stat& s) {
#ifdef __APPLE__
    return int64_t(s.st_mtimespec.tv_sec) * 1'000'000'000 + s.st_mtimespec.tv_nsec;
#else
    return int64_t(s.st_mtim.tv_sec) * 1'000'000'000 + s.st_mtim.tv_nsec;
#endif
}

Status statusOf(const struct stat& s) {
    Status status;
    if (S_ISREG(s.st_mode)) {
        status.kind = RCS_FILE_REGULAR;
        status.size = uint64_t(s.st_size);
    } else if (S_ISDIR(s.st_mode)) {
        status.kind = RCS_FILE_DIRECTORY;
    }
    status.modified = nanoseconds(s);
    return status;
}

// Access time left as it is, the modification time set.
void times(int64_t modified, struct timespec (&out)[2]) {
    out[0].tv_sec = 0;
    out[0].tv_nsec = UTIME_OMIT;
    int64_t seconds = modified / 1'000'000'000;
    int64_t rest = modified % 1'000'000'000;
    if (rest < 0) {
        rest += 1'000'000'000;
        --seconds;
    }
    out[1].tv_sec = time_t(seconds);
    out[1].tv_nsec = long(rest);
}

// O_NONBLOCK: a pipe opens at once (and is refused below) instead of
// waiting for a writer.
constexpr int readFlags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;

[[noreturn]] void cannotOpen(const std::string& path, int code = errno) {
    if (code == ELOOP) {
        fail(RCS_ERROR_INVALID_STATE, path + " is a symbolic link: not followed");
    }
    failed("Cannot open", path, code);
}

// What was opened with readFlags is a file, read as any other (waiting on the disk).
void readable(int fd, const std::string& path) {
    struct stat s {};
    if (fstat(fd, &s) != 0) {
        failed("Cannot look at", path);
    }
    if (!S_ISREG(s.st_mode)) {
        fail(RCS_ERROR_INVALID_STATE, path + " is not a file (a pipe, a device...)");
    }
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK) != 0) {
        failed("Cannot set up", path);
    }
}

} // namespace

struct Directory::Handle {
    explicit Handle(int descriptor) : fd(descriptor) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { ::close(fd); }
    const int fd;
};

bool absolute(std::string_view path) {
    return !path.empty() && path[0] == '/';
}

std::string join(std::string_view directory, std::string_view relative) {
    std::string path(directory);
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    if (path != "/") {
        path += '/';
    }
    return path + std::string(relative);
}

std::string lastName(std::string_view path) {
    while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    const size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string parentPath(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos || slash == 0 ? "/" : std::string(path.substr(0, slash));
}

std::optional<Status> status(const std::string& path, const std::string& shown) {
    struct stat s {};
    if (lstat(path.c_str(), &s) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return std::nullopt;
        }
        failed("Cannot look at", shown);
    }
    return statusOf(s);
}

uint64_t systemFreeSpace(const std::string& directory) {
    struct statvfs s {};
    if (statvfs(directory.c_str(), &s) != 0) {
        failed("Cannot learn the free space of", "the receiving directory");
    }
    return uint64_t(s.f_bavail) * uint64_t(s.f_frsize);
}

// ---- Directory ----------------------------------------------------------------------------

Directory Directory::open(const std::string& path, std::string shown, std::string below) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "No directory is there: " + shown);
        }
        failed("Cannot open", shown);
    }
    return Directory(std::make_shared<const Handle>(fd), std::move(shown), std::move(below));
}

std::optional<Directory> Directory::child(const std::string& name, bool make) const {
    const std::string path = under(name);
    constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int fd = openat(handle_->fd, name.c_str(), flags);
    if (fd < 0 && errno == ENOENT) {
        if (!make) {
            return std::nullopt;
        }
        // Made here, or by someone else meanwhile: what is there now is opened as any other.
        if (mkdirat(handle_->fd, name.c_str(), 0777) != 0 && errno != EEXIST) {
            failed("Cannot make the directory", path);
        }
        fd = openat(handle_->fd, name.c_str(), flags);
    }
    if (fd < 0) {
        if (errno == ELOOP) {
            fail(RCS_ERROR_INVALID_STATE, "A symbolic link is in the way (not followed): " + path);
        }
        if (errno == ENOTDIR) {
            fail(RCS_ERROR_INVALID_STATE, "A file (or something else that is no directory) is in the way: " + path);
        }
        failed("Cannot open the directory", path);
    }
    return Directory(std::make_shared<const Handle>(fd), path, path);
}

std::vector<std::string> Directory::list() const {
    // A description of its own, opened afresh (not followed): its position is not this one's.
    const int fd = openat(handle_->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* dir = fd < 0 ? nullptr : fdopendir(fd);
    if (!dir) {
        const int code = errno;
        if (fd >= 0) {
            ::close(fd);
        }
        failed("Cannot read the directory", shown_, code);
    }
    std::vector<std::string> names;
    errno = 0;
    while (const dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }
    const int code = errno;
    closedir(dir);
    if (code) {
        failed("Cannot read the directory", shown_, code);
    }
    std::sort(names.begin(), names.end());
    return names;
}

uint64_t Directory::volume() const {
    struct stat s {};
    if (fstat(handle_->fd, &s) != 0) {
        failed("Cannot look at", shown_);
    }
    return uint64_t(s.st_dev);
}

std::optional<Status> Directory::status(const std::string& name) const {
    struct stat s {};
    if (fstatat(handle_->fd, name.c_str(), &s, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return std::nullopt;
        }
        failed("Cannot look at", under(name));
    }
    return statusOf(s);
}

File Directory::read(const std::string& name, const std::string& shown) const {
    const std::string path = shown.empty() ? under(name) : shown;
    const int fd = openat(handle_->fd, name.c_str(), readFlags);
    if (fd < 0) {
        cannotOpen(path);
    }
    File file(fd, path);
    readable(fd, path);
    return file;
}

File Directory::create(const std::string& name) const {
    const std::string path = under(name);
    const int fd =
        openat(handle_->fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0) {
        if (errno == EEXIST) {
            fail(RCS_ERROR_INVALID_STATE, path + " is there already");
        }
        failed("Cannot make", path);
    }
    return File(fd, path);
}

File Directory::write(const std::string& name, uint64_t offset) const {
    const std::string path = under(name);
    // Not cut before it is known to be a file of its own: O_TRUNC would cut
    // what a hard link names. O_NONBLOCK: a pipe there fails at once.
    const int fd = openat(handle_->fd, name.c_str(),
                          O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (offset ? 0 : O_CREAT), 0666);
    if (fd < 0) {
        const int code = errno;
        if (code == ENOENT && offset) {
            fail(RCS_ERROR_INVALID_STATE, path + " is gone: " + std::to_string(offset) + " bytes of it were written");
        }
        if (code == ENXIO) {
            fail(RCS_ERROR_INVALID_STATE, path + " is a pipe, not a file");
        }
        cannotOpen(path, code);
    }
    File file(fd, path);
    struct stat s {};
    if (fstat(fd, &s) != 0) {
        failed("Cannot look at", path);
    }
    if (!S_ISREG(s.st_mode)) {
        fail(RCS_ERROR_INVALID_STATE, path + " is not a file");
    }
    if (s.st_nlink != 1) {
        fail(RCS_ERROR_INVALID_STATE, path + " has another name (a hard link): not written");
    }
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK) != 0) {
        failed("Cannot set up", path);
    }
    const auto size = uint64_t(s.st_size);
    if (size < offset) {
        fail(RCS_ERROR_INVALID_STATE, path + " holds " + std::to_string(size) + " bytes, not the " +
                                          std::to_string(offset) + " written before");
    }
    if (size > offset && ftruncate(fd, off_t(offset)) != 0) {
        failed("Cannot cut", path);
    }
    file.position_ = offset;
    return file;
}

bool Directory::rename(const std::string& from, const std::string& to, bool replace) const {
    const int fd = handle_->fd;
    if (replace) {
        if (renameat(fd, from.c_str(), fd, to.c_str()) != 0) {
            failed("Cannot move " + under(from) + " to", under(to));
        }
        return true;
    }
#if defined(__APPLE__)
    if (renameatx_np(fd, from.c_str(), fd, to.c_str(), RENAME_EXCL) == 0) {
        return true;
    }
#elif defined(__EMSCRIPTEN__)
    // The browser's file system is this process's own memory: nothing else
    // can create `to` between the look and the move.
    if (faccessat(fd, to.c_str(), F_OK, 0) == 0) {
        errno = EEXIST;
    } else if (renameat(fd, from.c_str(), fd, to.c_str()) == 0) {
        return true;
    }
#else
    // renameat2 by its number: Android's C library has it from Android 11 on, its kernels before.
    constexpr unsigned noReplace = 1; // RENAME_NOREPLACE.
    if (syscall(SYS_renameat2, fd, from.c_str(), fd, to.c_str(), noReplace) == 0) {
        return true;
    }
#endif
    if (errno == EEXIST) {
        return false;
    }
    failed("Cannot move " + under(from) + " to", under(to));
}

void Directory::remove(const std::string& name) const {
    if (unlinkat(handle_->fd, name.c_str(), 0) != 0 && errno != ENOENT) {
        failed("Cannot delete", under(name));
    }
}

void Directory::setModified(const std::string& name, int64_t modified) const {
    struct timespec t[2];
    times(modified, t);
    if (utimensat(handle_->fd, name.c_str(), t, AT_SYMLINK_NOFOLLOW) != 0) {
        failed("Cannot set the time of", under(name));
    }
}

// ---- File ---------------------------------------------------------------------------------

File::File(File&& other) noexcept
    : handle_(std::exchange(other.handle_, -1)), path_(std::move(other.path_)), position_(other.position_) {}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, -1);
        path_ = std::move(other.path_);
        position_ = other.position_;
    }
    return *this;
}

File::~File() {
    close();
}

void File::close() noexcept {
    if (handle_ >= 0) {
        ::close(int(handle_));
        handle_ = -1;
    }
}

Status File::status() const {
    struct stat s {};
    if (fstat(int(handle_), &s) != 0) {
        failed("Cannot look at", path_);
    }
    return statusOf(s);
}

void File::readAt(uint64_t offset, std::span<uint8_t> data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t got = pread(int(handle_), data.data() + done, data.size() - done, off_t(offset + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed("Cannot read", path_);
        }
        if (got == 0) {
            fail(RCS_ERROR_INVALID_STATE, path_ + " ends before " + std::to_string(offset + data.size()) +
                                              " bytes: it changed while it went");
        }
        done += size_t(got);
    }
}

void File::append(std::span<const uint8_t> data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t put = pwrite(int(handle_), data.data() + done, data.size() - done, off_t(position_));
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed("Cannot write", path_);
        }
        done += size_t(put);
        position_ += uint64_t(put);
    }
}

} // namespace rcsdk::file_transfer::files
