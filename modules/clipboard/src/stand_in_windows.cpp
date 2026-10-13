#include "stand_in_windows.hpp"

#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <set>
#include <thread>

#ifndef FD_UNICODE
#define FD_UNICODE 0x80000000
#endif
#ifndef COWAIT_DISPATCH_CALLS
#define COWAIT_DISPATCH_CALLS 0x8
#endif
#ifndef COWAIT_DISPATCH_WINDOW_MESSAGES
#define COWAIT_DISPATCH_WINDOW_MESSAGES 0x10
#endif

namespace rcsdk::clipboard {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto waitMost = 30s;          // For a listing, or a piece read.
constexpr auto idleClose = 5s;          // No stream of a paste open this long: it is through.
constexpr uint64_t piecesAhead = 32;    // Asked ahead of where a stream reads (8 MiB).
constexpr int64_t windowsEpoch = 116444736000000000; // 1601 to 1970, in 100 ns.

std::wstring wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}

FILETIME fileTime(int64_t modified) {
    const auto ticks = uint64_t(std::max<int64_t>(modified / 100 + windowsEpoch, 0));
    FILETIME time;
    time.dwLowDateTime = DWORD(ticks);
    time.dwHighDateTime = DWORD(ticks >> 32);
    return time;
}

HGLOBAL globalOf(const void* data, size_t size) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    if (memory) {
        std::memcpy(GlobalLock(memory), data, size);
        GlobalUnlock(memory);
    }
    return memory;
}

// Waits for `ready()` (checked under the fetch's lock), at most until
// `deadline`, dispatching COM calls and window messages meanwhile (the
// clipboard's STA thread waits here).
template <class Ready>
bool waitOn(PasteFetch& fetch, HANDLE event, Clock::time_point deadline, Ready ready) {
    for (;;) {
        {
            std::lock_guard lock(fetch.mutex);
            if (ready()) {
                return true;
            }
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            return false;
        }
        DWORD index = 0;
        CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES, DWORD(left), 1, &event,
                                 &index);
    }
}

// One paste: its fetch, what the descriptor lists, its streams.
struct Paste {
    explicit Paste(std::shared_ptr<PasteFetch> f) : fetch(std::move(f)), event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        std::lock_guard lock(fetch->mutex);
        fetch->wake = [handle = event] { SetEvent(handle); };
    }
    ~Paste() {
        {
            std::lock_guard lock(fetch->mutex);
            fetch->wake = nullptr;
        }
        CloseHandle(event);
    }
    const std::shared_ptr<PasteFetch> fetch;
    const HANDLE event;
    std::vector<uint32_t> items;    // The descriptor's entries: the fetch's items they are...
    std::vector<std::string> paths; // ...and their paths, as the descriptor gave them.
    // Under `mutex`: its streams open, when it was last used, whether it was closed.
    std::mutex mutex;
    int streams = 0;
    Clock::time_point used = Clock::now();
    bool closed = false;
    std::atomic<uint32_t> streamSerial{0};
};

// A file's contents as Explorer reads them: pieces of the fetch.
class ContentStream final : public IStream {
public:
    ContentStream(std::shared_ptr<Paste> paste, std::shared_ptr<Pastes> pastes, uint32_t item, FileItem file)
        : paste_(std::move(paste)), pastes_(std::move(pastes)), item_(item), file_(std::move(file)),
          serial_(uint64_t(++paste_->streamSerial) << 32) {
        std::lock_guard lock(paste_->mutex);
        ++paste_->streams;
        paste_->used = Clock::now();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) {
            {
                std::lock_guard lock(paste_->mutex);
                --paste_->streams;
                paste_->used = Clock::now();
            }
            {
                auto& answers = paste_->fetch->answers; // Its pieces asked ahead and not read.
                std::lock_guard lock(paste_->fetch->mutex);
                answers.erase(answers.lower_bound(serial_), answers.lower_bound(serial_ + (uint64_t(1) << 32)));
            }
            delete this;
        }
        return left;
    }

    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG size, ULONG* read) override {
        ULONG done = 0;
        auto& fetch = *paste_->fetch;
        while (done < size && position_ < file_.size) {
            const uint64_t piece = position_ / RCS_FILE_READ_MAX;
            if (have_ != piece) {
                ask(piece);
                const uint64_t tag = serial_ | piece;
                if (!waitOn(fetch, paste_->event, Clock::now() + waitMost,
                            [&] { return fetch.answers.count(tag) || fetch.ended; })) {
                    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                }
                rcs_result result = RCS_OK;
                Bytes bytes;
                {
                    std::lock_guard lock(fetch.mutex);
                    const auto answer = fetch.answers.find(tag);
                    if (answer == fetch.answers.end()) {
                        return E_FAIL; // The fetch ended.
                    }
                    result = answer->second.first;
                    bytes = std::move(answer->second.second);
                    // What it asked before and skipped (a seek) goes too.
                    fetch.answers.erase(fetch.answers.lower_bound(serial_), std::next(answer));
                }
                asked_.erase(piece);
                if (result == RCS_ERROR_QUEUE_FULL) { // Other streams' reads fill the fetch: a moment, and again.
                    waitOn(fetch, paste_->event, Clock::now() + 100ms, [] { return false; });
                    continue;
                }
                if (result != RCS_OK) {
                    return STG_E_READFAULT; // It changed at the other end, or cannot be read.
                }
                have_ = piece;
                bytes_ = std::move(bytes);
            }
            const uint64_t at = position_ - piece * RCS_FILE_READ_MAX;
            const auto n = ULONG(std::min<uint64_t>(size - done, bytes_.size() - at));
            std::memcpy(static_cast<uint8_t*>(buffer) + done, bytes_.data() + at, n);
            done += n;
            position_ += n;
        }
        {
            std::lock_guard lock(paste_->mutex);
            paste_->used = Clock::now();
        }
        if (read) {
            *read = done;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* where) override {
        const int64_t base = origin == STREAM_SEEK_SET   ? 0
                             : origin == STREAM_SEEK_CUR ? int64_t(position_)
                             : origin == STREAM_SEEK_END ? int64_t(file_.size)
                                                         : -1;
        if (base < 0 || base + move.QuadPart < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = uint64_t(base + move.QuadPart);
        if (where) {
            where->QuadPart = position_;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* to, ULARGE_INTEGER count, ULARGE_INTEGER* read,
                                     ULARGE_INTEGER* written) override {
        uint64_t moved = 0;
        std::vector<uint8_t> buffer(RCS_FILE_READ_MAX);
        HRESULT result = S_OK;
        while (moved < count.QuadPart) {
            ULONG got = 0;
            result = Read(buffer.data(), ULONG(std::min<uint64_t>(buffer.size(), count.QuadPart - moved)), &got);
            if (FAILED(result) || !got) {
                break;
            }
            ULONG put = 0;
            result = to->Write(buffer.data(), got, &put);
            moved += put;
            if (FAILED(result) || put != got) {
                break;
            }
        }
        if (read) {
            read->QuadPart = moved;
        }
        if (written) {
            written->QuadPart = moved;
        }
        return FAILED(result) ? result : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override {
        if (!stat) {
            return STG_E_INVALIDPOINTER;
        }
        *stat = STATSTG{};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = file_.size;
        stat->mtime = fileTime(file_.modified);
        stat->grfMode = STGM_READ;
        if (!(flags & STATFLAG_NONAME)) {
            const std::wstring name = wide(file_.path.substr(file_.path.rfind('/') + 1));
            stat->pwcsName = static_cast<wchar_t*>(CoTaskMemAlloc((name.size() + 1) * sizeof(wchar_t)));
            if (!stat->pwcsName) {
                return E_OUTOFMEMORY;
            }
            std::memcpy(stat->pwcsName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    // The piece read now and those after it, while not asked already.
    void ask(uint64_t piece) {
        const uint64_t last = (file_.size - 1) / RCS_FILE_READ_MAX;
        for (uint64_t k = piece; k <= std::min(last, piece + piecesAhead - 1); ++k) {
            if (k != have_ && asked_.insert(k).second) {
                const uint64_t offset = k * RCS_FILE_READ_MAX;
                pastes_->read(*paste_->fetch, item_, offset, uint32_t(std::min<uint64_t>(RCS_FILE_READ_MAX,
                                                                                         file_.size - offset)),
                              serial_ | k);
            }
        }
    }

    std::atomic<ULONG> refs_{1};
    const std::shared_ptr<Paste> paste_;
    const std::shared_ptr<Pastes> pastes_;
    const uint32_t item_;
    const FileItem file_;
    const uint64_t serial_; // Its tags: this and the piece's number.
    uint64_t position_ = 0;
    std::set<uint64_t> asked_;            // Pieces asked and not taken.
    uint64_t have_ = ~uint64_t(0);        // The piece in bytes_.
    Bytes bytes_;
};

class StandInObject final : public IDataObject {
public:
    StandInObject(Files files, std::vector<std::pair<UINT, Bytes>> formats, std::shared_ptr<Pastes> pastes)
        : files_(std::move(files)), formats_(std::move(formats)), pastes_(std::move(pastes)),
          descriptor_(RegisterClipboardFormatW(L"FileGroupDescriptorW")),
          contents_(RegisterClipboardFormatW(L"FileContents")) {
        const DWORD copy = DROPEFFECT_COPY; // Never a move: Explorer deletes nothing at the other end.
        formats_.emplace_back(RegisterClipboardFormatW(L"Preferred DropEffect"),
                              Bytes(reinterpret_cast<const uint8_t*>(&copy),
                                    reinterpret_cast<const uint8_t*>(&copy) + sizeof(copy)));
        closer_ = std::thread([this] { closeIdle(); });
    }
    ~StandInObject() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        closer_.join();
        close(paste_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) {
            delete this;
        }
        return left;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!format || !medium) {
            return E_INVALIDARG;
        }
        *medium = STGMEDIUM{};
        if (format->cfFormat == descriptor_ && (format->tymed & TYMED_HGLOBAL)) {
            return describe(medium);
        }
        if (format->cfFormat == contents_ && (format->tymed & TYMED_ISTREAM)) {
            return open(format->lindex, medium);
        }
        for (const auto& [id, bytes] : formats_) {
            if (format->cfFormat == id && (format->tymed & TYMED_HGLOBAL)) {
                medium->tymed = TYMED_HGLOBAL;
                medium->hGlobal = globalOf(bytes.data(), bytes.size());
                return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
            }
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) {
            return E_INVALIDARG;
        }
        if ((format->cfFormat == descriptor_ && (format->tymed & TYMED_HGLOBAL)) ||
            (format->cfFormat == contents_ && (format->tymed & TYMED_ISTREAM))) {
            return S_OK;
        }
        for (const auto& [id, bytes] : formats_) {
            if (format->cfFormat == id && (format->tymed & TYMED_HGLOBAL)) {
                return S_OK;
            }
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) {
            out->ptd = nullptr;
        }
        return DATA_S_SAMEFORMATETC;
    }
    // What Explorer tells as a paste ends (Performed DropEffect, Paste
    // Succeeded...) is taken and not acted on.
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM* medium, BOOL release) override {
        if (release && medium) {
            ReleaseStgMedium(medium);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        std::vector<FORMATETC> list;
        list.push_back({CLIPFORMAT(descriptor_), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        list.push_back({CLIPFORMAT(contents_), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM});
        for (const auto& [id, bytes] : formats_) {
            list.push_back({CLIPFORMAT(id), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        }
        return SHCreateStdEnumFmtEtc(UINT(list.size()), list.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    // The paste going, or a new one (its fetch started) where that one is through.
    std::shared_ptr<Paste> current() {
        std::unique_lock lock(mutex_);
        if (paste_) {
            std::lock_guard pasting(paste_->mutex);
            std::lock_guard fetching(paste_->fetch->mutex);
            if (!paste_->closed && !paste_->fetch->ended) {
                paste_->used = Clock::now();
                return paste_;
            }
        }
        lock.unlock();
        if (pastes_->revoked(files_.placing)) {
            pastes_->refused(files_.placing, RCS_ERROR_INVALID_STATE,
                             "The copy's files were withdrawn: the other end copied something else since");
            return nullptr;
        }
        auto made = std::make_shared<Paste>(pastes_->fetch(files_.placing, RCS_FILE_FETCH_READ));
        lock.lock();
        const auto old = std::exchange(paste_, made);
        lock.unlock();
        close(old);
        return made;
    }

    HRESULT describe(STGMEDIUM* medium) {
        const auto paste = current();
        if (!paste) {
            return E_FAIL;
        }
        auto& fetch = *paste->fetch;
        if (!waitOn(fetch, paste->event, Clock::now() + waitMost, [&] { return fetch.listed || fetch.ended; })) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        std::vector<FileItem> items;
        {
            std::lock_guard lock(fetch.mutex);
            if (!fetch.listed) {
                return E_FAIL; // The fetch ended before its items came: refused, or the other end left.
            }
            items = fetch.items;
        }
        std::vector<FILEDESCRIPTORW> entries;
        std::vector<uint32_t> map;
        for (uint32_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            std::wstring path = wide(item.path);
            std::replace(path.begin(), path.end(), L'/', L'\\');
            if (item.kind == RCS_FILE_OTHER) {
                continue;
            }
            if (path.size() >= MAX_PATH) {
                std::fprintf(stderr, "rcsdk clipboard: %s is left out of the paste: its path is too long\n",
                             item.path.c_str());
                continue;
            }
            FILEDESCRIPTORW entry{};
            entry.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI | FD_UNICODE;
            entry.dwFileAttributes = item.kind == RCS_FILE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            entry.nFileSizeLow = DWORD(item.size);
            entry.nFileSizeHigh = DWORD(item.size >> 32);
            entry.ftLastWriteTime = fileTime(item.modified);
            std::memcpy(entry.cFileName, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
            entries.push_back(entry);
            map.push_back(i);
        }
        {
            std::lock_guard lock(paste->mutex);
            paste->items = map;
            paste->paths.clear();
            for (const uint32_t i : map) {
                paste->paths.push_back(items[i].path);
            }
        }
        const size_t size = sizeof(UINT) + entries.size() * sizeof(FILEDESCRIPTORW);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
        if (!memory) {
            return E_OUTOFMEMORY;
        }
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(memory));
        group->cItems = UINT(entries.size());
        std::memcpy(group->fgd, entries.data(), entries.size() * sizeof(FILEDESCRIPTORW));
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }

    HRESULT open(LONG index, STGMEDIUM* medium) {
        std::shared_ptr<Paste> paste;
        {
            std::lock_guard lock(mutex_);
            paste = paste_;
        }
        if (!paste) { // Contents without the descriptor first: it is listed now.
            STGMEDIUM listed{};
            if (const HRESULT described = describe(&listed); FAILED(described)) {
                return described;
            }
            ReleaseStgMedium(&listed);
            std::lock_guard lock(mutex_);
            paste = paste_;
        }
        FileItem file;
        uint32_t item = 0;
        std::vector<std::string> paths;
        bool through = false;
        {
            std::lock_guard pasting(paste->mutex);
            std::lock_guard fetching(paste->fetch->mutex);
            if (index < 0 || size_t(index) >= paste->items.size()) {
                return DV_E_LINDEX;
            }
            item = paste->items[size_t(index)];
            if (item >= paste->fetch->items.size()) {
                return DV_E_LINDEX; // Not in the listing fetched again.
            }
            file = paste->fetch->items[item];
            through = paste->closed || paste->fetch->ended.has_value();
            paths = paste->paths;
        }
        if (through) {
            // The paste was through meanwhile (Explorer's question about a
            // name taken kept it waiting): fetched again, its files found by
            // the paths the descriptor gave.
            paste = current();
            if (!paste) {
                return E_FAIL;
            }
            auto& fetch = *paste->fetch;
            if (!waitOn(fetch, paste->event, Clock::now() + waitMost, [&] { return fetch.listed || fetch.ended; })) {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            std::lock_guard pasting(paste->mutex);
            std::lock_guard fetching(fetch.mutex);
            if (!fetch.listed) {
                return E_FAIL;
            }
            const auto at = [&](const std::string& path) {
                const auto found = std::find_if(fetch.items.begin(), fetch.items.end(),
                                                [&](const FileItem& listed) { return listed.path == path; });
                return found == fetch.items.end() ? ~uint32_t(0) : uint32_t(found - fetch.items.begin());
            };
            if (paste->paths.empty()) { // Not described again: the old descriptor's entries are its.
                paste->paths = paths;
                for (const auto& path : paths) {
                    paste->items.push_back(at(path));
                }
            }
            item = at(paths[size_t(index)]);
            if (item == ~uint32_t(0) || fetch.items[item].size != file.size) {
                return DV_E_LINDEX; // Gone, or changed, at the other end.
            }
            file = fetch.items[item];
        }
        if (file.kind != RCS_FILE_REGULAR) {
            return DV_E_LINDEX;
        }
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = new ContentStream(paste, pastes_, item, file);
        return S_OK;
    }

    // A paste through: its fetch closed (what was read whole is COMPLETED).
    void close(const std::shared_ptr<Paste>& paste) {
        if (!paste) {
            return;
        }
        {
            std::lock_guard lock(paste->mutex);
            if (paste->closed) {
                return;
            }
            paste->closed = true;
        }
        pastes_->close(*paste->fetch, RCS_OK);
    }

    // Closes the paste no stream of which has been open for idleClose.
    void closeIdle() {
        std::unique_lock lock(mutex_);
        while (!wake_.wait_for(lock, 1s, [&] { return quit_; })) {
            const auto paste = paste_;
            lock.unlock();
            bool idle = false;
            if (paste) {
                std::lock_guard pasting(paste->mutex);
                idle = !paste->closed && !paste->streams && Clock::now() - paste->used >= idleClose;
            }
            if (idle) {
                close(paste);
            }
            lock.lock();
        }
    }

    std::atomic<ULONG> refs_{1};
    const Files files_;
    std::vector<std::pair<UINT, Bytes>> formats_;
    const std::shared_ptr<Pastes> pastes_;
    const UINT descriptor_, contents_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    std::shared_ptr<Paste> paste_; // The newest.
    std::thread closer_;           // Last.
};

} // namespace

IDataObject* createStandIn(const Files& files, std::vector<std::pair<UINT, Bytes>> formats,
                           std::shared_ptr<Pastes> pastes) {
    return new StandInObject(files, std::move(formats), std::move(pastes));
}

} // namespace rcsdk::clipboard
