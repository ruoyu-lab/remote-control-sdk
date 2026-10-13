#include "pastes.hpp"

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rcsdk::clipboard {
namespace {

namespace fs = std::filesystem;

fs::path pathOf(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8Of(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string hex(const uint8_t* bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

} // namespace

Pastes::~Pastes() {
    stop();
}

void Pastes::configure(const std::string& directory, bool standIn) {
    std::lock_guard lock(mutex_);
    given_ = directory;
    standIn_ = standIn;
}

bool Pastes::standIn() const {
    std::lock_guard lock(mutex_);
    return standIn_;
}

std::string Pastes::directoryFor(const Publication& publication) {
    std::unique_lock lock(mutex_);
    std::error_code error;
    if (given_.empty() && own_.empty()) {
        uint8_t random[32];
        rcs_generate_connection_key(random, sizeof(random));
#ifdef _WIN32
        const auto process = uint64_t(GetCurrentProcessId());
#else
        const auto process = uint64_t(getpid());
#endif
        const fs::path made =
            fs::temp_directory_path(error) / ("rcsdk-clipboard-" + std::to_string(process) + "-" + hex(random, 6));
        if (error || !fs::create_directories(made, error)) {
            fail(RCS_ERROR_DEVICE, "No directory of the module's in the system's temporary one (" + error.message() +
                                       "): give the clipboard a files_directory");
        }
        fs::permissions(made, fs::perms::owner_all, error); // This account's alone.
        own_ = utf8Of(made);
    }
    const fs::path directory = pathOf(given_.empty() ? own_ : given_) / hex(publication.data(), publication.size());
    lock.unlock();
    fs::create_directories(directory, error);
    if (error) {
        fail(RCS_ERROR_DEVICE, "Cannot make " + utf8Of(directory) + ": " + error.message());
    }
    return utf8Of(directory);
}

bool Pastes::tell(const ToSession& message) {
    std::lock_guard lock(telling_);
    if (stopped_) {
        return false;
    }
    tell_(message);
    return true;
}

std::shared_ptr<PasteFetch> Pastes::fetch(uint64_t placing, rcs_file_fetch_mode mode, const std::string& directory,
                                          bool ahead) {
    std::shared_ptr<PasteFetch> made;
    {
        std::lock_guard lock(mutex_);
        made = std::make_shared<PasteFetch>(++serial_, placing);
        fetches_[made->id] = made;
    }
    if (!tell(WantMessage{made->id, placing, mode, directory, ahead})) {
        ended(EndedMessage{made->id, RCS_ERROR_DISCONNECTED, "The clipboard's machine half stopped", false});
    }
    return made;
}

void Pastes::read(const PasteFetch& fetch, uint32_t item, uint64_t offset, uint32_t size, uint64_t tag) {
    tell(ReadMessage{fetch.id, item, offset, size, tag});
}

void Pastes::close(const PasteFetch& fetch, rcs_result outcome) {
    tell(CloseMessage{fetch.id, outcome});
}

void Pastes::refused(uint64_t placing, rcs_result result, const std::string& why) {
    tell(RefusedMessage{placing, result, why});
}

bool Pastes::revoked(uint64_t placing) const {
    std::lock_guard lock(mutex_);
    return revoked_.count(placing) != 0;
}

void Pastes::onRevoked(std::function<void(uint64_t)> hook) {
    std::lock_guard lock(mutex_);
    revokedHook_ = std::move(hook);
}

std::shared_ptr<PasteFetch> Pastes::find(uint64_t request) {
    std::lock_guard lock(mutex_);
    const auto found = fetches_.find(request);
    return found == fetches_.end() ? nullptr : found->second;
}

void Pastes::changed(PasteFetch& fetch) {
    std::function<void()> wake;
    {
        std::lock_guard lock(fetch.mutex);
        wake = fetch.wake;
    }
    fetch.changed.notify_all();
    if (wake) {
        wake();
    }
}

void Pastes::listed(ListedMessage listed) {
    if (const auto fetch = find(listed.request)) {
        {
            std::lock_guard lock(fetch->mutex);
            fetch->items = std::move(listed.items);
            fetch->listed = true;
        }
        changed(*fetch);
    }
}

void Pastes::data(DataMessage data) {
    if (const auto fetch = find(data.request)) {
        {
            std::lock_guard lock(fetch->mutex);
            fetch->answers[data.tag] = {data.result, std::move(data.data)};
        }
        changed(*fetch);
    }
}

void Pastes::ended(EndedMessage ended) {
    std::shared_ptr<PasteFetch> fetch;
    {
        std::lock_guard lock(mutex_);
        const auto found = fetches_.find(ended.request);
        if (found == fetches_.end()) {
            return;
        }
        fetch = found->second;
        fetches_.erase(found);
    }
    {
        std::lock_guard lock(fetch->mutex);
        fetch->ended = std::move(ended);
    }
    changed(*fetch);
}

void Pastes::revoke(uint64_t placing) {
    std::function<void(uint64_t)> hook;
    {
        std::lock_guard lock(mutex_);
        revoked_.insert(placing);
        hook = revokedHook_;
    }
    if (hook) {
        hook(placing);
    }
}

void Pastes::stop() {
    {
        std::lock_guard lock(telling_);
        stopped_ = true;
    }
    std::map<uint64_t, std::shared_ptr<PasteFetch>> going;
    std::string own;
    {
        std::lock_guard lock(mutex_);
        going.swap(fetches_);
        own.swap(own_);
    }
    for (auto& [id, fetch] : going) {
        {
            std::lock_guard lock(fetch->mutex);
            fetch->ended = EndedMessage{id, RCS_ERROR_DISCONNECTED, "The clipboard's machine half stopped", false};
        }
        changed(*fetch);
    }
    if (!own.empty()) {
        std::error_code ignored;
        fs::remove_all(pathOf(own), ignored);
    }
}

} // namespace rcsdk::clipboard
