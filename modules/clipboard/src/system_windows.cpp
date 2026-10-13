// The system clipboard (SystemClipboard) on Windows. Text is
// CF_UNICODETEXT; HTML the registered "HTML Format" (CF_HTML: a header of
// byte offsets before the UTF-8 document); RTF "Rich Text Format"; an image
// the registered "PNG" format where a program put one, else the bitmap
// Windows makes of any (encoded as PNG with WIC), and it is placed as PNG
// beside a CF_DIBV5. A copy is concealed where its program asked to keep it
// out of the history or the cloud (password managers do), and placed with
// those marks.
//
// What this process places is owned by a message-only window on a thread of
// its own that answers the messages Windows sends the owner (another
// program's EmptyClipboard waits for them); the clipboard itself is opened
// and read on the caller's thread.
//
// Files (FILES) are CF_HDROP's paths (another program's virtual files alone
// are not read: FILES is left out), cut where "Preferred DropEffect" says
// move. Files placed whole are CF_HDROP; the other end's are virtual files
// (stand_in_windows.cpp), placed through OLE (OleSetClipboard) on the owner
// window's thread, which is OLE's single-threaded apartment for that. A
// process of LocalSystem (an agent that follows the secure desktop) does
// neither: it would read what the user may not, and Explorer, the user's,
// could not call its objects.
#include "stand_in_windows.hpp"
#include "system.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace rcsdk::clipboard {
namespace {

std::string errorText(DWORD code) {
    return "Windows error " + std::to_string(code);
}

// A COM interface held.
template <class T>
class Com {
public:
    T* value = nullptr;
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T** out() noexcept {
        reset();
        return &value;
    }
    T* operator->() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
    void reset() noexcept {
        if (value) {
            value->Release();
            value = nullptr;
        }
    }
};

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
        fail(RCS_ERROR_DEVICE, std::string(operation) + " failed (HRESULT " + code + ")");
    }
}

// UTF-8 to UTF-16; throws Error(INVALID_ARGUMENT) naming `what` for text that is not UTF-8.
std::wstring wide(std::string_view text, const char* what) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (size <= 0) {
        fail(RCS_ERROR_INVALID_ARGUMENT, std::string(what) + " is not UTF-8");
    }
    std::wstring result(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), size);
    return result;
}

// UTF-16 to UTF-8 (what is not UTF-16 becomes U+FFFD).
std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_t(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
    }
    return result;
}

[[noreturn]] void clipboardFailure(const std::string& what) {
    fail(RCS_ERROR_DEVICE, what + " (" + errorText(GetLastError()) + ")");
}

// The clipboard, open on this thread while this lives. Another program
// holds it for moments: tried for 200 ms.
class OpenClipboardScope {
public:
    explicit OpenClipboardScope(HWND owner) {
        for (int attempt = 0; !OpenClipboard(owner); ++attempt) {
            if (attempt == 20) {
                clipboardFailure("The clipboard is held open by another program");
            }
            Sleep(10);
        }
    }
    ~OpenClipboardScope() { CloseClipboard(); }
    OpenClipboardScope(const OpenClipboardScope&) = delete;
    OpenClipboardScope& operator=(const OpenClipboardScope&) = delete;
};

// A global memory block's bytes (GlobalSize may round up: text formats end
// at their NUL).
Bytes globalBytes(HANDLE data) {
    const SIZE_T size = GlobalSize(data);
    const void* memory = GlobalLock(data);
    if (!memory) {
        return {};
    }
    Bytes bytes(static_cast<const uint8_t*>(memory), static_cast<const uint8_t*>(memory) + size);
    GlobalUnlock(data);
    return bytes;
}

// Places `size` bytes of `data` in `format` (the clipboard owns the memory then).
void place(UINT format, const void* data, size_t size) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    if (!memory) {
        clipboardFailure("No memory for the clipboard");
    }
    void* target = GlobalLock(memory);
    std::memcpy(target, data, size);
    GlobalUnlock(memory);
    if (!SetClipboardData(format, memory)) {
        GlobalFree(memory);
        clipboardFailure("The clipboard did not take a format");
    }
}

// The bytes before the first NUL.
void cutAtNul(Bytes& bytes) {
    bytes.erase(std::find(bytes.begin(), bytes.end(), uint8_t(0)), bytes.end());
}

// ---- Text ---------------------------------------------------------------------------------

std::string withoutCarriageReturns(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        out += text[i];
    }
    return out;
}

std::wstring withCarriageReturns(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() + text.size() / 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
            out += L'\r';
        }
        out += text[i];
    }
    return out;
}

// ---- HTML ---------------------------------------------------------------------------------

// A header field's value ("StartHTML:0000000105"); -1 when absent.
long long headerValue(std::string_view header, std::string_view name) {
    const size_t at = header.find(name);
    if (at == std::string_view::npos) {
        return -1;
    }
    size_t i = at + name.size();
    bool negative = false;
    if (i < header.size() && header[i] == '-') {
        negative = true;
        ++i;
    }
    long long value = 0;
    size_t digits = 0;
    for (; i < header.size() && header[i] >= '0' && header[i] <= '9' && digits < 12; ++i, ++digits) {
        value = value * 10 + (header[i] - '0');
    }
    return digits && !negative ? value : -1;
}

// The document CF_HTML holds: between StartHTML and EndHTML, else its fragment.
std::string htmlDocument(const Bytes& bytes) {
    const std::string_view all(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::string_view header = all.substr(0, std::min<size_t>(all.size(), 512));
    for (const auto& [start, end] :
         {std::pair{"StartHTML:", "EndHTML:"}, std::pair{"StartFragment:", "EndFragment:"}}) {
        const long long from = headerValue(header, start), to = headerValue(header, end);
        if (from >= 0 && to > from && size_t(to) <= all.size()) {
            return std::string(all.substr(size_t(from), size_t(to - from)));
        }
    }
    return {};
}

size_t findNoCase(std::string_view text, std::string_view what, size_t from = 0) {
    const auto lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    };
    for (size_t i = from; i + what.size() <= text.size(); ++i) {
        size_t k = 0;
        while (k < what.size() && lower(text[i + k]) == what[k]) {
            ++k;
        }
        if (k == what.size()) {
            return i;
        }
    }
    return std::string_view::npos;
}

// CF_HTML of a document: the fragment is what its StartFragment and
// EndFragment comments mark, else its body, else the whole of it in a body.
std::string cfHtml(std::string_view html) {
    std::string document;
    size_t fragmentStart = 0, fragmentEnd = 0;
    const size_t startMark = html.find("<!--StartFragment-->"), endMark = html.find("<!--EndFragment-->");
    const size_t body = findNoCase(html, "<body");
    const size_t bodyOpen = body == std::string_view::npos ? body : html.find('>', body);
    const size_t bodyClose = findNoCase(html, "</body");
    if (startMark != std::string_view::npos && endMark != std::string_view::npos && startMark < endMark) {
        document = html;
        fragmentStart = startMark + std::string_view("<!--StartFragment-->").size();
        fragmentEnd = endMark;
    } else if (bodyOpen != std::string_view::npos && bodyClose != std::string_view::npos && bodyOpen < bodyClose) {
        document = html;
        fragmentStart = bodyOpen + 1;
        fragmentEnd = bodyClose;
    } else {
        document = "<html><body><!--StartFragment-->";
        fragmentStart = document.size();
        document += html;
        fragmentEnd = document.size();
        document += "<!--EndFragment--></body></html>";
    }
    const auto field = [](const char* name, size_t value) {
        char line[40];
        std::snprintf(line, sizeof(line), "%s%010zu\r\n", name, value);
        return std::string(line);
    };
    // The header's size does not depend on the values (ten digits each).
    std::string header = "Version:0.9\r\n" + field("StartHTML:", 0) + field("EndHTML:", 0) +
                         field("StartFragment:", 0) + field("EndFragment:", 0);
    const size_t offset = header.size();
    header = "Version:0.9\r\n" + field("StartHTML:", offset) + field("EndHTML:", offset + document.size()) +
             field("StartFragment:", offset + fragmentStart) + field("EndFragment:", offset + fragmentEnd);
    return header + document;
}

// ---- Images -------------------------------------------------------------------------------

class Imaging {
public:
    Imaging() {
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(IWICImagingFactory), reinterpret_cast<void**>(factory_.out())),
                   "Creating the WIC imaging factory");
    }

    // A bitmap Windows made of the copied image, as PNG (its alpha, which
    // bitmaps rarely carry right, ignored).
    Bytes pngOfBitmap(HBITMAP bitmap) {
        Com<IWICBitmap> source;
        check(factory_->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, source.out()),
                   "Reading the copied bitmap");
        Com<IWICFormatConverter> converted;
        check(factory_->CreateFormatConverter(converted.out()), "Creating a WIC converter");
        check(converted->Initialize(source.value, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom),
                   "Converting the copied bitmap");
        return encodePng(converted.value, GUID_WICPixelFormat24bppBGR);
    }

    // A PNG as a bottom-up 32-bit CF_DIBV5 with straight alpha; empty where
    // WIC cannot read it.
    Bytes dibOfPng(const Bytes& png) {
        Com<IWICStream> stream;
        Com<IWICBitmapDecoder> decoder;
        Com<IWICBitmapFrameDecode> frame;
        Com<IWICFormatConverter> converted;
        UINT width = 0, height = 0;
        if (FAILED(factory_->CreateStream(stream.out())) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), DWORD(png.size()))) ||
            FAILED(factory_->CreateDecoderFromStream(stream.value, nullptr, WICDecodeMetadataCacheOnDemand,
                                                     decoder.out())) ||
            FAILED(decoder->GetFrame(0, frame.out())) || FAILED(factory_->CreateFormatConverter(converted.out())) ||
            FAILED(converted->Initialize(frame.value, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)) ||
            FAILED(converted->GetSize(&width, &height)) || !width || !height ||
            uint64_t(width) * height > (uint64_t(256) << 20) / 4) {
            return {};
        }
        const UINT stride = width * 4;
        Bytes pixels(size_t(stride) * height);
        if (FAILED(converted->CopyPixels(nullptr, stride, UINT(pixels.size()), pixels.data()))) {
            return {};
        }
        BITMAPV5HEADER header{};
        header.bV5Size = sizeof(header);
        header.bV5Width = LONG(width);
        header.bV5Height = LONG(height); // Bottom-up, as most programs read them.
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5SizeImage = DWORD(pixels.size());
        header.bV5RedMask = 0x00FF0000;
        header.bV5GreenMask = 0x0000FF00;
        header.bV5BlueMask = 0x000000FF;
        header.bV5AlphaMask = 0xFF000000;
        header.bV5CSType = LCS_sRGB;
        header.bV5Intent = LCS_GM_IMAGES;
        Bytes dib(sizeof(header) + pixels.size());
        std::memcpy(dib.data(), &header, sizeof(header));
        for (UINT row = 0; row < height; ++row) {
            std::memcpy(dib.data() + sizeof(header) + size_t(row) * stride,
                        pixels.data() + size_t(height - 1 - row) * stride, stride);
        }
        return dib;
    }

private:
    Bytes encodePng(IWICBitmapSource* source, WICPixelFormatGUID format) {
        Com<IStream> stream;
        check(CreateStreamOnHGlobal(nullptr, TRUE, stream.out()), "Creating a memory stream");
        Com<IWICBitmapEncoder> encoder;
        Com<IWICBitmapFrameEncode> frame;
        UINT width = 0, height = 0;
        check(factory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.out()), "Creating a PNG encoder");
        check(encoder->Initialize(stream.value, WICBitmapEncoderNoCache), "Starting the PNG");
        check(encoder->CreateNewFrame(frame.out(), nullptr), "Adding the PNG's picture");
        check(frame->Initialize(nullptr), "Starting the PNG's picture");
        check(source->GetSize(&width, &height), "Reading the picture's size");
        check(frame->SetSize(width, height), "Sizing the PNG");
        check(frame->SetPixelFormat(&format), "Choosing the PNG's pixels");
        check(frame->WriteSource(source, nullptr), "Encoding the PNG");
        check(frame->Commit(), "Finishing the PNG's picture");
        check(encoder->Commit(), "Finishing the PNG");
        STATSTG stat{};
        check(stream->Stat(&stat, STATFLAG_NONAME), "Measuring the PNG");
        HGLOBAL memory = nullptr;
        check(GetHGlobalFromStream(stream.value, &memory), "Reading the PNG");
        const auto* bytes = static_cast<const uint8_t*>(GlobalLock(memory));
        Bytes png(bytes, bytes + size_t(stat.cbSize.QuadPart));
        GlobalUnlock(memory);
        return png;
    }

    Com<IWICImagingFactory> factory_;
};

bool isPng(const Bytes& bytes) {
    static constexpr uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return bytes.size() > sizeof(signature) && std::equal(std::begin(signature), std::end(signature), bytes.begin());
}

// ---- Files ----------------------------------------------------------------------------------

constexpr int64_t windowsEpoch = 116444736000000000; // 1601 to 1970, in 100 ns.

// The files CF_HDROP names, as they are now (a link, or what is gone, is
// left out); nullopt without any.
std::optional<Files> filesOf(HANDLE data) {
    const auto drop = static_cast<HDROP>(data);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    Files files;
    for (UINT i = 0; i < count; ++i) {
        const UINT size = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(size_t(size) + 1, L'\0');
        path.resize(DragQueryFileW(drop, i, path.data(), size + 1));
        const std::wstring look = path.size() >= MAX_PATH && path.rfind(L"\\\\", 0) != 0 ? L"\\\\?\\" + path : path;
        WIN32_FIND_DATAW found{};
        const HANDLE find = FindFirstFileW(look.c_str(), &found);
        if (find == INVALID_HANDLE_VALUE) {
            continue;
        }
        FindClose(find);
        const bool link = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (found.dwReserved0 & 0x20000000);
        if (link) {
            continue; // A symbolic link or a junction (a name surrogate): not followed.
        }
        FileEntry entry;
        entry.path = narrow(path);
        entry.name = narrow(found.cFileName);
        entry.kind = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? RCS_FILE_DIRECTORY : RCS_FILE_REGULAR;
        entry.size = entry.kind == RCS_FILE_REGULAR ? (uint64_t(found.nFileSizeHigh) << 32) | found.nFileSizeLow : 0;
        const auto ticks = int64_t((uint64_t(found.ftLastWriteTime.dwHighDateTime) << 32) |
                                   found.ftLastWriteTime.dwLowDateTime);
        entry.modified = (ticks - windowsEpoch) * 100;
        files.entries.push_back(std::move(entry));
    }
    return files.entries.empty() ? std::nullopt : std::optional(std::move(files));
}

// CF_HDROP of files placed whole.
Bytes dropOf(const Files& files) {
    std::wstring paths;
    for (const auto& entry : files.entries) {
        std::wstring path = wide(entry.path, "A file's path");
        std::replace(path.begin(), path.end(), L'/', L'\\');
        paths += path;
        paths += L'\0';
    }
    paths += L'\0';
    DROPFILES header{};
    header.pFiles = sizeof(header);
    header.fWide = TRUE;
    Bytes bytes(sizeof(header) + paths.size() * sizeof(wchar_t));
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), paths.data(), paths.size() * sizeof(wchar_t));
    return bytes;
}

Bytes dword(DWORD value) {
    const auto* begin = reinterpret_cast<const uint8_t*>(&value);
    return Bytes(begin, begin + sizeof(value));
}

// ---- The owner window -----------------------------------------------------------------------

// A message-only window on a thread of its own that answers what Windows
// sends the clipboard's owner, until this goes.
class OwnerWindow {
public:
    static constexpr UINT runMessage = WM_APP + 1; // lParam: a std::function<void()> to call.

    OwnerWindow() {
        thread_ = std::thread([this] { run(); });
        std::unique_lock lock(mutex_);
        made_.wait(lock, [&] { return done_; });
        if (!window_) {
            thread_.join();
            fail(RCS_ERROR_UNSUPPORTED,
                 "This process cannot own the clipboard: no window (" + errorText(error_) + ")");
        }
    }
    ~OwnerWindow() {
        PostMessageW(window_, WM_CLOSE, 0, 0);
        thread_.join();
    }
    OwnerWindow(const OwnerWindow&) = delete;
    OwnerWindow& operator=(const OwnerWindow&) = delete;
    HWND get() const noexcept { return window_; }
    // Calls `task` on its thread (OLE's there), and waits for it.
    void call(const std::function<void()>& task) {
        SendMessageW(window_, runMessage, 0, reinterpret_cast<LPARAM>(&task));
    }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        if (message == runMessage) {
            (*reinterpret_cast<const std::function<void()>*>(l))();
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, w, l);
    }

    void run() {
        const bool ole = SUCCEEDED(OleInitialize(nullptr)); // A single-threaded apartment, for OleSetClipboard.
        static const wchar_t* const name = L"RcsdkClipboardOwner";
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.lpfnWndProc = &procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = name;
        RegisterClassExW(&type); // Once per process; again it fails, and the class is there.
        HWND window = CreateWindowExW(0, name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
        {
            std::lock_guard lock(mutex_);
            window_ = window;
            error_ = window ? 0 : GetLastError();
            done_ = true;
        }
        made_.notify_all();
        if (window) {
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (ole) {
            OleUninitialize();
        }
    }

    std::mutex mutex_;
    std::condition_variable made_;
    bool done_ = false;
    HWND window_ = nullptr;
    DWORD error_ = 0;
    std::thread thread_;
};

// ---- The clipboard --------------------------------------------------------------------------

class WindowsClipboard final : public SystemClipboard {
public:
    WindowsClipboard()
        : apartment_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))),
          html_(RegisterClipboardFormatW(L"HTML Format")), rtf_(RegisterClipboardFormatW(L"Rich Text Format")),
          png_(RegisterClipboardFormatW(L"PNG")),
          exclude_(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing")),
          history_(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory")),
          cloud_(RegisterClipboardFormatW(L"CanUploadToCloudClipboard")),
          descriptor_(RegisterClipboardFormatW(L"FileGroupDescriptorW")),
          dropEffect_(RegisterClipboardFormatW(L"Preferred DropEffect")) {}
    ~WindowsClipboard() override {
        // The other end's files placed last, still on the clipboard: what
        // was beside them stays, the files (whose pastes would ask a machine
        // half that is gone) go. Never flushed: that would read every file.
        if (standIn_) {
            bool current = false;
            owner_.call([&] {
                current = OleIsCurrentClipboard(standIn_.value) == S_OK;
                if (current) {
                    OleSetClipboard(nullptr);
                }
            });
            standIn_.reset();
            if (current && !beside_.empty()) {
                try {
                    OpenClipboardScope open(owner_.get());
                    for (const auto& [format, bytes] : beside_) {
                        place(format, bytes.data(), bytes.size());
                    }
                } catch (const Error&) {
                }
            }
        }
        imaging_.reset();
        if (apartment_) {
            CoUninitialize();
        }
    }

    uint64_t changeCount() override { return GetClipboardSequenceNumber(); }

    Content read(uint32_t formats, size_t maxBytes) override {
        OpenClipboardScope open(owner_.get());
        Content content;
        content.concealed = IsClipboardFormatAvailable(exclude_) || dwordIsZero(history_) || dwordIsZero(cloud_);
        size_t left = maxBytes;
        const auto add = [&](rcs_clipboard_format format, Bytes bytes) {
            if (bytes.empty()) {
                return;
            }
            if (bytes.size() > left) {
                content.leftOut |= 1u << format;
                return;
            }
            left -= bytes.size();
            content.items.push_back({format, std::move(bytes)});
        };
        const auto wanted = [&](rcs_clipboard_format format, bool available) {
            if (available && !(formats & (1u << format))) {
                content.leftOut |= 1u << format;
            }
            return available && (formats & (1u << format));
        };
        if (wanted(RCS_CLIPBOARD_TEXT, IsClipboardFormatAvailable(CF_UNICODETEXT))) {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                const SIZE_T size = GlobalSize(data);
                if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                    const size_t length = wcsnlen(text, size / sizeof(wchar_t));
                    const std::string utf8 = withoutCarriageReturns(narrow(std::wstring_view(text, length)));
                    GlobalUnlock(data);
                    add(RCS_CLIPBOARD_TEXT, Bytes(utf8.begin(), utf8.end()));
                }
            }
        }
        if (wanted(RCS_CLIPBOARD_HTML, html_ && IsClipboardFormatAvailable(html_))) {
            if (HANDLE data = GetClipboardData(html_)) {
                auto bytes = globalBytes(data);
                cutAtNul(bytes);
                const std::string document = htmlDocument(bytes);
                if (utf8Characters(document)) {
                    add(RCS_CLIPBOARD_HTML, Bytes(document.begin(), document.end()));
                }
            }
        }
        if (wanted(RCS_CLIPBOARD_RTF, rtf_ && IsClipboardFormatAvailable(rtf_))) {
            if (HANDLE data = GetClipboardData(rtf_)) {
                auto bytes = globalBytes(data);
                cutAtNul(bytes);
                add(RCS_CLIPBOARD_RTF, std::move(bytes));
            }
        }
        const bool png = png_ && IsClipboardFormatAvailable(png_);
        if (wanted(RCS_CLIPBOARD_IMAGE, png || IsClipboardFormatAvailable(CF_BITMAP))) {
            Bytes image;
            if (HANDLE data = png ? GetClipboardData(png_) : nullptr) {
                if (GlobalSize(data) > left) {
                    content.leftOut |= 1u << RCS_CLIPBOARD_IMAGE;
                } else {
                    image = globalBytes(data);
                }
            }
            if (image.empty() && !(content.leftOut & (1u << RCS_CLIPBOARD_IMAGE))) {
                if (auto* bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP))) {
                    image = imaging().pngOfBitmap(bitmap);
                }
            }
            if (!image.empty() && isPng(image)) {
                add(RCS_CLIPBOARD_IMAGE, std::move(image));
            }
        }
        // Files: CF_HDROP's; another program's virtual files alone are not read.
        const bool drop = IsClipboardFormatAvailable(CF_HDROP);
        if (wanted(RCS_CLIPBOARD_FILES, drop || (descriptor_ && IsClipboardFormatAvailable(descriptor_)))) {
            auto files = drop ? filesOf(GetClipboardData(CF_HDROP)) : std::nullopt;
            if (files && filesBytes(*files) <= left) {
                left -= filesBytes(*files);
                if (HANDLE effect = IsClipboardFormatAvailable(dropEffect_) ? GetClipboardData(dropEffect_) : nullptr) {
                    const Bytes bytes = globalBytes(effect);
                    DWORD value = 0;
                    if (bytes.size() >= sizeof(value)) {
                        std::memcpy(&value, bytes.data(), sizeof(value));
                    }
                    files->cut = (value & DROPEFFECT_MOVE) != 0;
                }
                content.files = std::move(files);
            } else {
                content.leftOut |= filesFormat;
            }
        }
        return content;
    }

    uint64_t write(const Content& content) override {
        // Converted before the clipboard is opened: it stays open for moments only.
        std::vector<std::pair<UINT, Bytes>> data;
        for (const auto& item : content.items) {
            const std::string_view text(reinterpret_cast<const char*>(item.bytes.data()), item.bytes.size());
            switch (item.format) {
            case RCS_CLIPBOARD_TEXT: {
                const std::wstring utf16 = withCarriageReturns(wide(text, "The copied text"));
                const auto* begin = reinterpret_cast<const uint8_t*>(utf16.c_str());
                data.emplace_back(CF_UNICODETEXT, Bytes(begin, begin + (utf16.size() + 1) * sizeof(wchar_t)));
                break;
            }
            case RCS_CLIPBOARD_HTML: {
                const std::string html = cfHtml(text);
                Bytes bytes(html.begin(), html.end());
                bytes.push_back(0);
                data.emplace_back(html_, std::move(bytes));
                break;
            }
            case RCS_CLIPBOARD_RTF: {
                Bytes bytes = item.bytes;
                bytes.push_back(0);
                data.emplace_back(rtf_, std::move(bytes));
                break;
            }
            case RCS_CLIPBOARD_IMAGE: {
                data.emplace_back(png_, item.bytes);
                if (Bytes dib = imaging().dibOfPng(item.bytes); !dib.empty()) {
                    data.emplace_back(CF_DIBV5, std::move(dib));
                }
                break;
            }
            default:
                break;
            }
        }
        // Concealed copies, and files (whose names a history would keep, or
        // whose stand-in a clipboard manager would ask for): kept out of the
        // history and the cloud.
        if (content.concealed || content.files) {
            data.emplace_back(exclude_, dword(0));
            data.emplace_back(history_, dword(0));
            data.emplace_back(cloud_, dword(0));
        }
        standIn_.reset();
        beside_.clear();
        if (content.files && content.files->entries.front().path.empty()) {
            return placeStandIn(*content.files, std::move(data));
        }
        if (content.files) { // The files themselves: any program pastes them.
            data.emplace_back(CF_HDROP, dropOf(*content.files));
            data.emplace_back(dropEffect_, dword(DROPEFFECT_COPY));
        }
        {
            OpenClipboardScope open(owner_.get());
            if (!EmptyClipboard()) {
                clipboardFailure("The clipboard could not be emptied");
            }
            for (const auto& [format, bytes] : data) {
                place(format, bytes.data(), bytes.size());
            }
        }
        return GetClipboardSequenceNumber();
    }

private:
    // The other end's files: a data object of virtual files, placed through
    // OLE on the owner window's thread, `formats` beside them.
    uint64_t placeStandIn(const Files& files, std::vector<std::pair<UINT, Bytes>> formats) {
        beside_ = formats;
        Com<IDataObject> object;
        *object.out() = createStandIn(files, std::move(formats), pastes_);
        HRESULT placed = E_FAIL;
        owner_.call([&] { placed = OleSetClipboard(object.value); });
        if (FAILED(placed)) {
            check(placed, "Placing the files on the clipboard (OleSetClipboard)");
        }
        object.value->AddRef();
        *standIn_.out() = object.value;
        return GetClipboardSequenceNumber();
    }

    // A DWORD format that says 0 (CanIncludeInClipboardHistory, CanUploadToCloudClipboard).
    bool dwordIsZero(UINT format) {
        if (!format || !IsClipboardFormatAvailable(format)) {
            return false;
        }
        HANDLE data = GetClipboardData(format);
        const auto bytes = data ? globalBytes(data) : Bytes{};
        DWORD value = 1;
        if (bytes.size() >= sizeof(value)) {
            std::memcpy(&value, bytes.data(), sizeof(value));
        }
        return value == 0;
    }

    Imaging& imaging() {
        if (!imaging_) {
            imaging_ = std::make_unique<Imaging>();
        }
        return *imaging_;
    }

    const bool apartment_;
    const UINT html_, rtf_, png_, exclude_, history_, cloud_, descriptor_, dropEffect_;
    OwnerWindow owner_;
    std::unique_ptr<Imaging> imaging_;
    Com<IDataObject> standIn_;                  // The other end's files placed last...
    std::vector<std::pair<UINT, Bytes>> beside_; // ...and what was placed beside them.
};

} // namespace

std::unique_ptr<SystemClipboard> createPlatformClipboard() {
    return std::make_unique<WindowsClipboard>();
}

uint32_t platformFormats() noexcept {
    return allFormats;
}

bool platformCarriesFiles() noexcept {
    return true;
}

uint32_t platformFiles(std::string& why) {
    bool system = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        alignas(TOKEN_USER) uint8_t buffer[256];
        DWORD size = 0;
        if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
            system = IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, WinLocalSystemSid);
        }
        CloseHandle(token);
    }
    if (system) {
        why = "This process runs as LocalSystem (an agent that follows the secure desktop): it would read files the "
              "user may not, and Explorer could not call its objects; run the agent as the user (RCS_HELPER_USER)";
        return 0;
    }
    return RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
}

} // namespace rcsdk::clipboard
