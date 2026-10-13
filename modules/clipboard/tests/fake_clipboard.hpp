// A clipboard of the tests' (clipboard::setFactory): what it holds, its
// change count, how often it was written, and reads that fail as if another
// program held it open. Files copied are paths of the test's; the other
// end's files placed are a stand-in that pastes ask Pastes for, as a
// system's paste does: into a directory (as Finder's), or read file by file
// (as Explorer's).
#pragma once

#include <algorithm>
#include "pastes.hpp"
#include "system.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace rcsdk::clipboard::test {

struct FakeClipboard {
    std::mutex mutex;
    Content content;
    uint64_t count = 1;
    int writes = 0;
    std::atomic<int> busyReads{0};
    std::shared_ptr<Pastes> pastes; // Of the half that placed what it holds.

    // A program copies `copy`.
    void copy(Content copy) {
        std::lock_guard lock(mutex);
        content = std::move(copy);
        ++count;
    }
    Content held() {
        std::lock_guard lock(mutex);
        return content;
    }
    // The text it holds, empty without.
    std::string text() {
        std::lock_guard lock(mutex);
        for (const auto& item : content.items) {
            if (item.format == RCS_CLIPBOARD_TEXT) {
                return std::string(item.bytes.begin(), item.bytes.end());
            }
        }
        return {};
    }
    int written() {
        std::lock_guard lock(mutex);
        return writes;
    }

    // A paste of the other end's files it holds, into `directory`, as
    // Finder's: how the fetch ended (nullopt: it holds none, or nothing came
    // within the time).
    std::optional<EndedMessage> pasteInto(const std::string& directory,
                                          std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto fetch = start(RCS_FILE_FETCH_INTO, directory);
        if (!fetch || !fetch->waitFor([&] { return fetch->ended.has_value(); }, timeout)) {
            return std::nullopt;
        }
        std::lock_guard lock(fetch->mutex);
        return fetch->ended;
    }

    // A paste read file by file, as Explorer's: each file's bytes by its
    // path (`skip`: one left unread), and how the fetch ended once closed.
    struct Read {
        std::map<std::string, std::string> files;
        std::vector<FileItem> items;
        std::optional<EndedMessage> ended;
        rcs_result failed = RCS_OK; // A read that did not succeed.
    };
    std::optional<Read> pasteRead(const std::string& skip = {},
                                  std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto fetch = start(RCS_FILE_FETCH_READ, {});
        if (!fetch || !fetch->waitFor([&] { return fetch->listed || fetch->ended; }, timeout)) {
            return std::nullopt;
        }
        Read read;
        {
            std::lock_guard lock(fetch->mutex);
            read.items = fetch->items;
        }
        uint64_t tag = 0;
        for (uint32_t i = 0; i < read.items.size() && read.failed == RCS_OK; ++i) {
            const auto& item = read.items[i];
            if (item.kind != RCS_FILE_REGULAR || item.path == skip) {
                continue;
            }
            auto& bytes = read.files[item.path];
            // Piece by piece, as a stream is read; a piece asked twice is answered twice.
            for (uint64_t offset = 0; offset < item.size && read.failed == RCS_OK;) {
                const uint32_t size = uint32_t(std::min<uint64_t>(RCS_FILE_READ_MAX, item.size - offset));
                pastes->read(*fetch, i, offset, size, ++tag);
                if (!fetch->waitFor([&] { return fetch->answers.count(tag) || fetch->ended; }, timeout)) {
                    read.failed = RCS_ERROR_TIMEOUT;
                    break;
                }
                std::lock_guard lock(fetch->mutex);
                const auto answer = fetch->answers.find(tag);
                if (answer == fetch->answers.end()) {
                    read.failed = fetch->ended->result;
                    break;
                }
                read.failed = answer->second.first;
                bytes.append(answer->second.second.begin(), answer->second.second.end());
                offset += answer->second.second.size();
                fetch->answers.erase(answer);
            }
        }
        pastes->close(*fetch, read.failed);
        if (fetch->waitFor([&] { return fetch->ended.has_value(); }, timeout)) {
            std::lock_guard lock(fetch->mutex);
            read.ended = fetch->ended;
        }
        return read;
    }

private:
    std::shared_ptr<PasteFetch> start(rcs_file_fetch_mode mode, const std::string& directory) {
        std::shared_ptr<Pastes> broker;
        uint64_t placing = 0;
        {
            std::lock_guard lock(mutex);
            if (!content.files || !content.files->entries.front().path.empty() || !pastes) {
                return nullptr; // No stand-in: nothing of the other end's, or the files themselves.
            }
            broker = pastes;
            placing = content.files->placing;
        }
        return broker->fetch(placing, mode, directory);
    }
};

class FakeClipboardAccess final : public SystemClipboard {
public:
    explicit FakeClipboardAccess(FakeClipboard& board) : board_(board) {}
    uint64_t changeCount() override {
        std::lock_guard lock(board_.mutex);
        return board_.count;
    }
    Content read(uint32_t formats, size_t maxBytes) override {
        if (board_.busyReads > 0) {
            --board_.busyReads;
            fail(RCS_ERROR_DEVICE, "The clipboard is held open by another program (a test's)");
        }
        std::lock_guard lock(board_.mutex);
        return fitting(board_.content, formats, maxBytes);
    }
    uint64_t write(const Content& content) override {
        std::lock_guard lock(board_.mutex);
        board_.content = content;
        board_.content.leftOut = 0;
        board_.pastes = pastes_;
        ++board_.writes;
        return ++board_.count;
    }

private:
    FakeClipboard& board_;
};

inline Item clipboardItem(rcs_clipboard_format format, const std::string& bytes) {
    return {format, Bytes(bytes.begin(), bytes.end())};
}

// An image item whose bytes start as a 1 x 1 PNG does (what a copy is
// checked for), padded to `size` bytes.
inline Item pngItem(size_t size) {
    std::string png("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\0\x01\0\0\0\x01", 24);
    png.resize(std::max(size, png.size()), 'p');
    return clipboardItem(RCS_CLIPBOARD_IMAGE, png);
}

inline Content textCopy(const std::string& text, bool concealed = false) {
    Content content;
    content.items.push_back(clipboardItem(RCS_CLIPBOARD_TEXT, text));
    content.concealed = concealed;
    return content;
}

// A copy of files of the test's (full paths: kind, size and time as given).
inline Content filesCopy(std::vector<FileEntry> entries, bool cut = false) {
    Content content;
    content.files = Files{cut, {}, std::move(entries), 0};
    return content;
}

} // namespace rcsdk::clipboard::test
