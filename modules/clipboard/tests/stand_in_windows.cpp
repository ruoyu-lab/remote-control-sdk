// Windows: the other end's files as virtual files, the data object itself
// (no clipboard: nobody else's is touched). Its descriptor lists what the
// fetch listed; each file's stream reads it piece by piece, from anywhere;
// a paste no stream of which is open for 5 s is closed; files withdrawn are
// pasted no more. With RCSDK_TEST_SHELL_DROP=1 the shell (Explorer's copy
// engine) also takes the object as a drop on a folder of the test's and
// writes the files there: that shows its progress window, so it is opt-in.
#include "fake_clipboard.hpp"
#include "stand_in_windows.hpp"
#include "support/check.hpp"

#include <shlobj.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

using namespace rcsdk::clipboard;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// The session's half as the stand-in's pastes see it: a fetch lists these
// items, and reads answer from their contents, at once.
struct FakeSession {
    std::vector<FileItem> items{{"photos", RCS_FILE_DIRECTORY, 0, 0},
                                {"photos/a.jpg", RCS_FILE_REGULAR, 600000, 0},
                                {"photos/empty.txt", RCS_FILE_REGULAR, 0, 0},
                                {"notes.txt", RCS_FILE_REGULAR, 5, 0}};
    std::map<uint32_t, std::string> contents;
    std::mutex mutex;
    std::vector<ToSession> told;
    std::weak_ptr<Pastes> pastes;

    FakeSession() {
        std::string photo(600000, '\0');
        for (size_t i = 0; i < photo.size(); ++i) {
            photo[i] = char(i * 7 + 3);
        }
        contents = {{1, photo}, {2, ""}, {3, "notes"}};
    }
    void tell(const ToSession& message) {
        {
            std::lock_guard lock(mutex);
            told.push_back(message);
        }
        const auto broker = pastes.lock();
        if (const auto* want = std::get_if<WantMessage>(&message)) {
            broker->listed(ListedMessage{want->request, items});
        } else if (const auto* read = std::get_if<ReadMessage>(&message)) {
            const std::string& all = contents.at(read->item);
            const std::string piece = all.substr(size_t(read->offset), read->size);
            broker->data(DataMessage{read->request, read->tag, RCS_OK, Bytes(piece.begin(), piece.end())});
        } else if (const auto* close = std::get_if<CloseMessage>(&message)) {
            broker->ended(EndedMessage{close->request, RCS_OK, {}, true});
        }
    }
    template <class T>
    size_t count() {
        std::lock_guard lock(mutex);
        return size_t(std::count_if(told.begin(), told.end(), [](const ToSession& m) { return std::holds_alternative<T>(m); }));
    }
};

FORMATETC formatOf(const wchar_t* name, DWORD tymed, LONG index = -1) {
    return {CLIPFORMAT(RegisterClipboardFormatW(name)), nullptr, DVASPECT_CONTENT, index, tymed};
}

std::string readAll(IStream* stream, ULONG piece) {
    std::string out;
    std::vector<char> buffer(piece);
    for (;;) {
        ULONG got = 0;
        CHECK(SUCCEEDED(stream->Read(buffer.data(), piece, &got)));
        if (!got) {
            return out;
        }
        out.append(buffer.data(), got);
    }
}

void standIn() {
    CHECK(SUCCEEDED(OleInitialize(nullptr)));
    {
        FakeSession session;
        auto pastes = std::make_shared<Pastes>([&](const ToSession& message) { session.tell(message); });
        session.pastes = pastes;
        Files files = test::filesCopy({{"photos", RCS_FILE_DIRECTORY, 0, 0, ""}, {"notes.txt", RCS_FILE_REGULAR, 5, 0, ""}})
                          .files.value();
        files.placing = 3;
        const std::string text = "photos notes.txt";
        IDataObject* object = createStandIn(files, {{CF_TEXT, Bytes(text.begin(), text.end() + 1)}}, pastes);

        // The descriptor: what the fetch listed, as Windows' relative paths.
        FORMATETC descriptor = formatOf(L"FileGroupDescriptorW", TYMED_HGLOBAL);
        CHECK(object->QueryGetData(&descriptor) == S_OK);
        STGMEDIUM medium{};
        CHECK(object->GetData(&descriptor, &medium) == S_OK && medium.tymed == TYMED_HGLOBAL);
        const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
        CHECK(group->cItems == 4 && std::wstring(group->fgd[1].cFileName) == L"photos\\a.jpg" &&
              (group->fgd[0].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && group->fgd[1].nFileSizeLow == 600000);
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        CHECK(session.count<WantMessage>() == 1);

        // A file's stream: read whole, in pieces of any size; from elsewhere; its name and size.
        FORMATETC contents = formatOf(L"FileContents", TYMED_ISTREAM, 1);
        CHECK(object->GetData(&contents, &medium) == S_OK && medium.tymed == TYMED_ISTREAM);
        IStream* stream = medium.pstm;
        CHECK(readAll(stream, 100000) == session.contents[1]);
        LARGE_INTEGER middle{};
        middle.QuadPart = 300001;
        CHECK(stream->Seek(middle, STREAM_SEEK_SET, nullptr) == S_OK);
        CHECK(readAll(stream, 7) == session.contents[1].substr(300001));
        STATSTG stat{};
        CHECK(stream->Stat(&stat, STATFLAG_DEFAULT) == S_OK && stat.cbSize.QuadPart == 600000 &&
              std::wstring(stat.pwcsName) == L"a.jpg");
        CoTaskMemFree(stat.pwcsName);
        stream->Release();
        FORMATETC empty = formatOf(L"FileContents", TYMED_ISTREAM, 2), notes = formatOf(L"FileContents", TYMED_ISTREAM, 3);
        CHECK(object->GetData(&empty, &medium) == S_OK && readAll(medium.pstm, 10).empty());
        medium.pstm->Release();
        CHECK(object->GetData(&notes, &medium) == S_OK && readAll(medium.pstm, 10) == "notes");
        medium.pstm->Release();
        FORMATETC directory = formatOf(L"FileContents", TYMED_ISTREAM, 0);
        CHECK(object->GetData(&directory, &medium) == DV_E_LINDEX);

        // No stream open for 5 s: the paste is through, its fetch closed.
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!session.count<CloseMessage>() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(50ms);
        }
        CHECK(session.count<CloseMessage>() == 1);

        // Beside the files, what was placed with them; never a move.
        FORMATETC plain{CF_TEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        CHECK(object->GetData(&plain, &medium) == S_OK);
        ReleaseStgMedium(&medium);
        FORMATETC effect = formatOf(L"Preferred DropEffect", TYMED_HGLOBAL);
        CHECK(object->GetData(&effect, &medium) == S_OK &&
              *static_cast<const DWORD*>(GlobalLock(medium.hGlobal)) == DROPEFFECT_COPY);
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);

        if (const char* value = std::getenv("RCSDK_TEST_SHELL_DROP"); value && std::string(value) == "1") {
            // Explorer's copy engine takes it as a drop on a folder of the test's.
            const fs::path target = fs::temp_directory_path() / ("rcsdk-test-drop-" + std::to_string(GetCurrentProcessId()));
            fs::create_directories(target);
            PIDLIST_ABSOLUTE folder = nullptr;
            CHECK(SUCCEEDED(SHParseDisplayName(target.wstring().c_str(), nullptr, &folder, 0, nullptr)));
            IShellFolder* parent = nullptr;
            PCUITEMID_CHILD child = nullptr;
            CHECK(SUCCEEDED(SHBindToParent(folder, IID_IShellFolder, reinterpret_cast<void**>(&parent), &child)));
            IDropTarget* drop = nullptr;
            CHECK(SUCCEEDED(parent->GetUIObjectOf(nullptr, 1, &child, IID_IDropTarget, nullptr,
                                                  reinterpret_cast<void**>(&drop))));
            DWORD allowed = DROPEFFECT_COPY;
            const POINTL point{0, 0};
            CHECK(SUCCEEDED(drop->DragEnter(object, MK_LBUTTON, point, &allowed)));
            allowed = DROPEFFECT_COPY;
            CHECK(SUCCEEDED(drop->Drop(object, MK_LBUTTON, point, &allowed)));
            const auto copied = [&] {
                std::ifstream in(target / "photos" / "a.jpg", std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), {}) == session.contents[1];
            };
            const auto until = std::chrono::steady_clock::now() + 30s;
            while (!copied() && std::chrono::steady_clock::now() < until) {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    DispatchMessageW(&message);
                }
                std::this_thread::sleep_for(50ms);
            }
            CHECK(copied() && fs::exists(target / "photos" / "empty.txt"));
            drop->Release();
            parent->Release();
            CoTaskMemFree(folder);
            std::error_code ignored;
            fs::remove_all(target, ignored);
        }

        // Withdrawn: a new paste fails, and says why.
        pastes->revoke(3);
        std::this_thread::sleep_for(6s); // The paste going is through first.
        CHECK(object->GetData(&descriptor, &medium) == E_FAIL && session.count<RefusedMessage>() == 1);
        object->Release();
    }
    OleUninitialize();
}

} // namespace

int main() {
    return runTest("clipboard.stand_in_windows", standIn);
}
