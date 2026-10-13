// The file system on Windows: wide paths with \\?\ before them (any length),
// symbolic links and junctions (name surrogates) never followed; cloud
// placeholders and other reparse points are the files they stand for. A
// Directory is a handle held open without FILE_SHARE_DELETE, with those of
// the directories above it down from the one the host gave: a directory on
// the way cannot be renamed or deleted while it is used. What is in it is
// opened relative to it (NtCreateFile with RootDirectory), reparse points
// opened as themselves (FILE_OPEN_REPARSE_POINT) and refused where they are
// links, and renamed relative to it (FileRenameInformation with
// RootDirectory, which SetFileInformationByHandle does not take). A name
// opens what has it in any case, and also what has it as its short (8.3)
// name: what a transfer names it gives or replaces only where it is the
// long name there (a directory on the way too), else the item fails.
#include "files.hpp"

#include <windows.h>
#include <winternl.h>

namespace rcsdk::file_transfer::files {
namespace {

constexpr int64_t windowsEpoch = 116444736000000000; // 1601 to 1970, in 100 ns.
constexpr DWORD nameSurrogate = 0x20000000;          // IsReparseTagNameSurrogate.

// NtCreateFile's (ntdll, found at run time: no import library needed).
constexpr ULONG caseInsensitive = 0x00000040;
constexpr ULONG fileOpen = 1, fileCreate = 2, fileOpenIf = 3;
constexpr ULONG directoryFile = 0x00000001, nonDirectoryFile = 0x00000040;
constexpr ULONG synchronousNonalert = 0x00000020, openReparsePoint = 0x00200000;
constexpr LONG nameNotFound = LONG(0xC0000034), nameCollision = LONG(0xC0000035);
constexpr LONG isDirectory = LONG(0xC00000BA), notDirectory = LONG(0xC0000103);
using NtCreateFileFunction = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                          ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtSetInformationFileFunction = LONG(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
using NtQueryDirectoryFileFunction = LONG(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG,
                                                  BOOLEAN, PUNICODE_STRING, BOOLEAN);
using DosErrorFunction = ULONG(NTAPI*)(LONG);
constexpr ULONG fileRenameInformation = 10;
constexpr ULONG fileBothDirectoryInformation = 3;
constexpr LONG noSuchFile = LONG(0xC000000F), noMoreFiles = LONG(0x80000006);

// FILE_BOTH_DIR_INFORMATION (ntifs.h): an entry of a directory, with its long
// name (FileName) and its short one.
struct BothDirectoryInformation {
    ULONG nextEntryOffset, fileIndex;
    LARGE_INTEGER creationTime, lastAccessTime, lastWriteTime, changeTime, endOfFile, allocationSize;
    ULONG fileAttributes, fileNameLength, eaSize;
    CCHAR shortNameLength;
    WCHAR shortName[12];
    WCHAR fileName[1];
};

std::wstring wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (size <= 0) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "A path that is not UTF-8");
    }
    std::wstring out(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), size);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string systemMessage(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = size ? narrow({text, size}) : "error " + std::to_string(code);
    LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
}

[[noreturn]] void failed(const std::string& what, const std::string& path, DWORD code = GetLastError()) {
    fail(RCS_ERROR_DEVICE, what + " " + path + ": " + systemMessage(code));
}

// A full path with \\?\ before it; messages call it `shown`.
std::wstring native(const std::string& path, const std::string& shown) {
    std::wstring w = wide(path);
    for (auto& c : w) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (w.rfind(L"\\\\?\\", 0) == 0) {
        return w;
    }
    const DWORD needed = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (!needed) {
        failed("Cannot make a full path of", shown);
    }
    std::wstring full(needed, L'\0');
    full.resize(GetFullPathNameW(w.c_str(), needed, full.data(), nullptr));
    if (full.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + full.substr(2);
    }
    return L"\\\\?\\" + full;
}

int64_t nanoseconds(const FILETIME& time) {
    const auto ticks = int64_t((uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime);
    return (ticks - windowsEpoch) * 100;
}

FILETIME fileTime(int64_t modified) {
    int64_t ticks = modified / 100 + windowsEpoch;
    if (modified % 100 < 0) {
        --ticks;
    }
    ticks = std::max<int64_t>(ticks, 0);
    FILETIME time;
    time.dwLowDateTime = DWORD(uint64_t(ticks));
    time.dwHighDateTime = DWORD(uint64_t(ticks) >> 32);
    return time;
}

// A symbolic link or a junction (a reparse point that names another place).
bool linkAt(DWORD attributes, const std::wstring& path) {
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return false;
    }
    WIN32_FIND_DATAW data{};
    const HANDLE found = FindFirstFileExW(path.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (found == INVALID_HANDLE_VALUE) {
        return true; // What it is cannot be told: not followed.
    }
    FindClose(found);
    return (data.dwReserved0 & nameSurrogate) != 0;
}

Status statusOf(DWORD attributes, bool link, uint64_t size, const FILETIME& written) {
    Status status;
    if (link || (attributes & FILE_ATTRIBUTE_DEVICE)) {
        status.kind = RCS_FILE_OTHER;
    } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        status.kind = RCS_FILE_DIRECTORY;
    } else {
        status.kind = RCS_FILE_REGULAR;
        status.size = size;
    }
    status.modified = nanoseconds(written);
    return status;
}

HANDLE handleOf(intptr_t handle) {
    return reinterpret_cast<HANDLE>(handle);
}

template <class Function>
Function ntdll(const char* name) {
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name)));
}

// The Win32 error of an NTSTATUS.
DWORD dosError(LONG status) {
    static const auto convert = ntdll<DosErrorFunction>("RtlNtStatusToDosError");
    return convert(status);
}

struct Opening {
    ACCESS_MASK access;
    ULONG options = 0;
    ULONG disposition = fileOpen;
    ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
};

// Opens `name`, one component, in the directory `parent`, as a handle to
// do I/O in order on, readable for its attributes as CreateFileW makes it.
// Returns the NTSTATUS (negative when it failed).
LONG openIn(HANDLE parent, const std::string& name, const Opening& how, HANDLE& out) {
    static const auto create = ntdll<NtCreateFileFunction>("NtCreateFile");
    std::wstring text = wide(name);
    UNICODE_STRING string{USHORT(text.size() * sizeof(wchar_t)), USHORT(text.size() * sizeof(wchar_t)), text.data()};
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &string;
    attributes.Attributes = caseInsensitive;
    IO_STATUS_BLOCK block{};
    return create(&out, how.access | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &attributes, &block, nullptr, FILE_ATTRIBUTE_NORMAL, how.share,
                  how.disposition, how.options | synchronousNonalert, nullptr, 0);
}

// Whether what `handle` is open on is a symbolic link or a junction.
bool linkOf(HANDLE handle, const std::string& path) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        failed("Cannot look at", path);
    }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
        failed("Cannot look at", path);
    }
    return (tag.ReparseTag & nameSurrogate) != 0;
}

// Whether `name`, one component, is another name of what is at it in
// `directory`: its short (8.3) name, its long one differing (other than in
// case). False where nothing is there. Asked through a handle of its own on
// the directory: a handle's first query fixes what its later ones match.
bool aliased(HANDLE directory, const std::string& name, const std::string& path) {
    static const auto query = ntdll<NtQueryDirectoryFileFunction>("NtQueryDirectoryFile");
    HANDLE own = nullptr;
    const LONG opened = openIn(directory, {}, {FILE_LIST_DIRECTORY, directoryFile}, own);
    if (opened < 0) {
        failed("Cannot look at", path, dosError(opened));
    }
    const struct Closed {
        HANDLE handle;
        ~Closed() { CloseHandle(handle); }
    } closed{own};
    std::wstring wanted = wide(name);
    UNICODE_STRING pattern{USHORT(wanted.size() * sizeof(wchar_t)), USHORT(wanted.size() * sizeof(wchar_t)),
                           wanted.data()};
    std::vector<uint64_t> entry(512); // 4 KiB, aligned: room for a name of 255 characters.
    IO_STATUS_BLOCK block{};
    const LONG status = query(own, nullptr, nullptr, nullptr, &block, entry.data(), ULONG(entry.size() * 8),
                              fileBothDirectoryInformation, TRUE, &pattern, TRUE);
    if (status == noSuchFile || status == noMoreFiles) {
        return false;
    }
    if (status < 0) {
        failed("Cannot look at", path, dosError(status));
    }
    const auto* found = reinterpret_cast<const BothDirectoryInformation*>(entry.data());
    return CompareStringOrdinal(found->fileName, int(found->fileNameLength / sizeof(wchar_t)), wanted.data(),
                                int(wanted.size()), TRUE) != CSTR_EQUAL;
}

} // namespace

struct Directory::Handle {
    Handle(HANDLE opened, std::shared_ptr<const Handle> above) : handle(opened), parent(std::move(above)) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { CloseHandle(handle); }
    const HANDLE handle;
    const std::shared_ptr<const Handle> parent; // Held open: a directory on the way cannot be moved.
};

bool absolute(std::string_view path) {
    const auto separator = [](char c) { return c == '\\' || c == '/'; };
    if (path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && separator(path[2])) {
        return true; // C:\...
    }
    return path.size() >= 3 && separator(path[0]) && separator(path[1]); // \\server\share\..., \\?\...
}

std::string join(std::string_view directory, std::string_view relative) {
    std::string path(directory);
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
        path.pop_back();
    }
    path += '\\';
    for (const char c : relative) {
        path += c == '/' ? '\\' : c;
    }
    return path;
}

std::string lastName(std::string_view path) {
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
        path.remove_suffix(1);
    }
    const size_t separator = path.find_last_of("\\/");
    std::string name(separator == std::string_view::npos ? path : path.substr(separator + 1));
    return name.find(':') == std::string::npos ? name : std::string(); // "C:" is a root.
}

std::string parentPath(std::string_view path) {
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
        path.remove_suffix(1);
    }
    const size_t separator = path.find_last_of("\\/");
    std::string parent(path.substr(0, separator == std::string_view::npos ? 0 : separator));
    return parent.size() == 2 && parent[1] == ':' ? parent + "\\" : parent; // "C:\".
}

std::optional<Status> status(const std::string& path, const std::string& shown) {
    const std::wstring n = native(path, shown);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(n.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return std::nullopt;
        }
        failed("Cannot look at", shown, code);
    }
    return statusOf(data.dwFileAttributes, linkAt(data.dwFileAttributes, n),
                    (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow, data.ftLastWriteTime);
}

uint64_t systemFreeSpace(const std::string& directory) {
    const std::string shown = "the receiving directory";
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW((native(directory, shown) + L"\\").c_str(), &available, nullptr, nullptr)) {
        failed("Cannot learn the free space of", shown);
    }
    return available.QuadPart;
}

// ---- Directory ----------------------------------------------------------------------------

Directory Directory::open(const std::string& path, std::string shown, std::string below) {
    // Links on the way are followed (the host chose this path); shared for
    // reading and writing, not for deleting or renaming.
    const HANDLE handle = CreateFileW(
        native(path, shown).c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_DIRECTORY ||
            code == ERROR_INVALID_NAME || code == ERROR_BAD_PATHNAME) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "No directory is there: " + shown);
        }
        failed("Cannot open", shown, code);
    }
    auto held = std::make_shared<const Handle>(handle, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        failed("Cannot look at", shown);
    }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "No directory is there: " + shown);
    }
    return Directory(std::move(held), std::move(shown), std::move(below));
}

std::vector<std::string> Directory::list() const {
    // Read through the handle, from its start (whatever its path leads to now).
    std::vector<uint64_t> buffer(8192); // 64 KiB, aligned for FILE_FULL_DIR_INFO.
    std::vector<std::string> names;
    for (auto kind = FileFullDirectoryRestartInfo;; kind = FileFullDirectoryInfo) {
        if (!GetFileInformationByHandleEx(handle_->handle, kind, buffer.data(), DWORD(buffer.size() * 8))) {
            const DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_FILES) {
                break;
            }
            failed("Cannot read the directory", shown_, code);
        }
        for (auto* at = reinterpret_cast<const uint8_t*>(buffer.data());;) {
            const auto* entry = reinterpret_cast<const FILE_FULL_DIR_INFO*>(at);
            const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                names.push_back(narrow(name));
            }
            if (!entry->NextEntryOffset) {
                break;
            }
            at += entry->NextEntryOffset;
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

uint64_t Directory::volume() const {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle_->handle, &info)) {
        failed("Cannot look at", shown_);
    }
    return info.dwVolumeSerialNumber;
}

std::optional<Directory> Directory::child(const std::string& name, bool make) const {
    const std::string path = under(name);
    HANDLE handle = nullptr;
    // Made here, or by someone else meanwhile: then opened as any other.
    const LONG status = openIn(handle_->handle, name,
                               {FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                                directoryFile | openReparsePoint, make ? fileOpenIf : fileOpen,
                                FILE_SHARE_READ | FILE_SHARE_WRITE},
                               handle);
    if (status == nameNotFound && !make) {
        return std::nullopt;
    }
    if (status == notDirectory) {
        fail(RCS_ERROR_INVALID_STATE, "A file (or something else that is no directory) is in the way: " + path);
    }
    if (status < 0) {
        failed(make ? "Cannot make the directory" : "Cannot open the directory", path, dosError(status));
    }
    auto held = std::make_shared<const Handle>(handle, handle_);
    if (linkOf(handle, path)) {
        fail(RCS_ERROR_INVALID_STATE, "A symbolic link (a junction too) is in the way (not followed): " + path);
    }
    if (aliased(handle_->handle, name, path)) {
        fail(RCS_ERROR_INVALID_STATE, path + " is the short (8.3) name of a directory named otherwise: not taken");
    }
    return Directory(std::move(held), path, path);
}

std::optional<Status> Directory::status(const std::string& name) const {
    const std::string path = under(name);
    HANDLE handle = nullptr;
    const LONG opened = openIn(handle_->handle, name, {FILE_READ_ATTRIBUTES, openReparsePoint}, handle);
    if (opened == nameNotFound) {
        return std::nullopt;
    }
    if (opened < 0) {
        failed("Cannot look at", path, dosError(opened));
    }
    File file(reinterpret_cast<intptr_t>(handle), path);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        failed("Cannot look at", path);
    }
    return statusOf(info.dwFileAttributes, linkOf(handle, path),
                    (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow, info.ftLastWriteTime);
}

File Directory::read(const std::string& name, const std::string& shown) const {
    const std::string path = shown.empty() ? under(name) : shown;
    HANDLE handle = nullptr;
    const LONG opened = openIn(handle_->handle, name,
                               {FILE_READ_DATA | FILE_READ_ATTRIBUTES, nonDirectoryFile | openReparsePoint}, handle);
    if (opened < 0) {
        failed("Cannot open", path, dosError(opened));
    }
    File file(reinterpret_cast<intptr_t>(handle), path);
    if (linkOf(handle, path)) {
        fail(RCS_ERROR_INVALID_STATE, path + " is a link: not followed");
    }
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        fail(RCS_ERROR_INVALID_STATE, path + " is not a file");
    }
    return file;
}

File Directory::create(const std::string& name) const {
    const std::string path = under(name);
    HANDLE handle = nullptr;
    // FILE_CREATE fails for anything there, a link too (it does not follow it).
    const LONG opened = openIn(handle_->handle, name,
                               {GENERIC_WRITE, nonDirectoryFile, fileCreate, FILE_SHARE_READ | FILE_SHARE_DELETE},
                               handle);
    if (opened == nameCollision) {
        fail(RCS_ERROR_INVALID_STATE, path + " is there already");
    }
    if (opened < 0) {
        failed("Cannot make", path, dosError(opened));
    }
    return File(reinterpret_cast<intptr_t>(handle), path);
}

File Directory::write(const std::string& name, uint64_t offset) const {
    const std::string path = under(name);
    HANDLE handle = nullptr;
    // Not cut before it is known to be a file of its own (a disposition that
    // cuts would cut what a hard link names); a link there is opened as
    // itself, to be refused below.
    const LONG opened = openIn(handle_->handle, name,
                               {GENERIC_WRITE, nonDirectoryFile | openReparsePoint, offset ? fileOpen : fileOpenIf,
                                FILE_SHARE_READ | FILE_SHARE_DELETE},
                               handle);
    if (opened == nameNotFound && offset) {
        fail(RCS_ERROR_INVALID_STATE, path + " is gone: " + std::to_string(offset) + " bytes of it were written");
    }
    if (opened == isDirectory) {
        fail(RCS_ERROR_INVALID_STATE, path + " is not a file");
    }
    if (opened < 0) {
        failed("Cannot open", path, dosError(opened));
    }
    File file(reinterpret_cast<intptr_t>(handle), path);
    if (linkOf(handle, path)) {
        fail(RCS_ERROR_INVALID_STATE, path + " is a link (or a device): not followed");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        failed("Cannot look at", path);
    }
    if (GetFileType(handle) != FILE_TYPE_DISK || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        fail(RCS_ERROR_INVALID_STATE, path + " is not a file");
    }
    if (info.nNumberOfLinks != 1) {
        fail(RCS_ERROR_INVALID_STATE, path + " has another name (a hard link): not written");
    }
    const uint64_t size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size < offset) {
        fail(RCS_ERROR_INVALID_STATE, path + " holds " + std::to_string(size) + " bytes, not the " +
                                          std::to_string(offset) + " written before");
    }
    LARGE_INTEGER at{};
    at.QuadPart = LONGLONG(offset);
    if (size > offset && (!SetFilePointerEx(handle, at, nullptr, FILE_BEGIN) || !SetEndOfFile(handle))) {
        failed("Cannot cut", path);
    }
    file.position_ = offset;
    return file;
}

bool Directory::rename(const std::string& from, const std::string& to, bool replace) const {
    if (replace && aliased(handle_->handle, to, under(to))) {
        fail(RCS_ERROR_INVALID_STATE, under(to) + " is the short (8.3) name of a file named otherwise: not replaced");
    }
    HANDLE handle = nullptr;
    const LONG opened =
        openIn(handle_->handle, from, {DELETE | FILE_READ_ATTRIBUTES, openReparsePoint}, handle);
    if (opened < 0) {
        failed("Cannot move " + under(from) + " to", under(to), dosError(opened));
    }
    const File file(reinterpret_cast<intptr_t>(handle), under(from));
    // Relative to this directory: wherever its path leads now, the new name is in it.
    static const auto set = ntdll<NtSetInformationFileFunction>("NtSetInformationFile");
    const std::wstring name = wide(to);
    std::vector<uint8_t> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = replace;
    info->RootDirectory = handle_->handle;
    info->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, name.data(), info->FileNameLength);
    IO_STATUS_BLOCK block{};
    const LONG status = set(handle, &block, info, ULONG(buffer.size()), fileRenameInformation);
    if (status >= 0) {
        return true;
    }
    if (!replace && status == nameCollision) {
        return false;
    }
    failed("Cannot move " + under(from) + " to", under(to), dosError(status));
}

void Directory::remove(const std::string& name) const {
    HANDLE handle = nullptr;
    const LONG opened =
        openIn(handle_->handle, name, {DELETE | FILE_READ_ATTRIBUTES, nonDirectoryFile | openReparsePoint}, handle);
    if (opened == nameNotFound) {
        return;
    }
    if (opened < 0) {
        failed("Cannot delete", under(name), dosError(opened));
    }
    const File file(reinterpret_cast<intptr_t>(handle), under(name));
    FILE_DISPOSITION_INFO info{TRUE};
    if (!SetFileInformationByHandle(handle, FileDispositionInfo, &info, sizeof(info))) {
        failed("Cannot delete", under(name));
    }
}

void Directory::setModified(const std::string& name, int64_t modified) const {
    HANDLE handle = nullptr;
    const LONG opened = openIn(handle_->handle, name, {FILE_WRITE_ATTRIBUTES, openReparsePoint}, handle);
    if (opened < 0) {
        failed("Cannot set the time of", under(name), dosError(opened));
    }
    const File file(reinterpret_cast<intptr_t>(handle), under(name));
    const FILETIME time = fileTime(modified);
    if (!SetFileTime(handle, nullptr, nullptr, &time)) {
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
    if (handle_ != -1) {
        CloseHandle(handleOf(handle_));
        handle_ = -1;
    }
}

Status File::status() const {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handleOf(handle_), &info)) {
        failed("Cannot look at", path_);
    }
    return statusOf(info.dwFileAttributes, false, (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
                    info.ftLastWriteTime);
}

void File::readAt(uint64_t offset, std::span<uint8_t> data) {
    size_t done = 0;
    while (done < data.size()) {
        OVERLAPPED at{};
        const uint64_t from = offset + done;
        at.Offset = DWORD(from);
        at.OffsetHigh = DWORD(from >> 32);
        DWORD got = 0;
        if (!ReadFile(handleOf(handle_), data.data() + done, DWORD(data.size() - done), &got, &at)) {
            const DWORD code = GetLastError();
            if (code != ERROR_HANDLE_EOF) {
                failed("Cannot read", path_, code);
            }
        }
        if (got == 0) {
            fail(RCS_ERROR_INVALID_STATE, path_ + " ends before " + std::to_string(offset + data.size()) +
                                              " bytes: it changed while it went");
        }
        done += got;
    }
}

void File::append(std::span<const uint8_t> data) {
    size_t done = 0;
    while (done < data.size()) {
        OVERLAPPED at{};
        at.Offset = DWORD(position_);
        at.OffsetHigh = DWORD(position_ >> 32);
        DWORD put = 0;
        if (!WriteFile(handleOf(handle_), data.data() + done, DWORD(data.size() - done), &put, &at)) {
            failed("Cannot write", path_);
        }
        done += put;
        position_ += put;
    }
}

} // namespace rcsdk::file_transfer::files
