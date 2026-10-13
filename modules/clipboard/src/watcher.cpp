#include "watcher.hpp"

#include <optional>

namespace rcsdk::clipboard {
namespace {

// A clipboard another program keeps open this many looks in a row is given up on for that copy.
constexpr int lookAttempts = 4;

} // namespace

Watcher::Watcher(rcs_role role, Use use, Handlers handlers, std::shared_ptr<Pastes> pastes)
    : role_(role), handlers_(std::move(handlers)), pastes_(std::move(pastes)), use_(use) {
    thread_ = std::thread([this] { run(); });
}

Watcher::~Watcher() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void Watcher::use(const Use& use) {
    {
        std::lock_guard lock(mutex_);
        if (use == use_) {
            return;
        }
        use_ = use;
        ++useSerial_;
    }
    wake_.notify_all();
}

void Watcher::place(uint64_t id, Content content) {
    {
        std::lock_guard lock(mutex_);
        writes_.push_back({id, std::move(content), false});
    }
    wake_.notify_all();
}

void Watcher::replace(uint64_t id, Content content) {
    {
        std::lock_guard lock(mutex_);
        writes_.push_back({id, std::move(content), true});
    }
    wake_.notify_all();
}

void Watcher::run() noexcept {
    uint64_t lookedSerial = 0; // The use the last look was made under.
    bool watching = false;     // A look was made since the watch began.
    std::unique_lock lock(mutex_);
    while (!quit_) {
        if (!writes_.empty()) {
            const Write next = std::move(writes_.front());
            writes_.pop_front();
            lock.unlock();
            write(next);
            lock.lock();
            continue;
        }
        const Use use = use_;
        const uint64_t serial = useSerial_;
        if (!use.watch) {
            watching = false;
            wake_.wait(lock, [&] { return quit_ || !writes_.empty() || useSerial_ != serial; });
            continue;
        }
        lock.unlock();
        look(use, serial != lookedSerial, !watching);
        lookedSerial = serial;
        watching = true;
        lock.lock();
        wake_.wait_for(lock, std::chrono::milliseconds(use.intervalMs),
                       [&] { return quit_ || !writes_.empty() || useSerial_ != serial; });
    }
    lock.unlock();
    clipboard_.reset(); // On the thread that made it.
}

bool Watcher::open() {
    if (clipboard_) {
        return true;
    }
    if (!unavailable_.empty()) {
        return false;
    }
    try {
        clipboard_ = createClipboard(role_);
        clipboard_->usePastes(pastes_);
        return true;
    } catch (const std::exception& error) {
        unavailable_ = error.what();
        return false;
    }
}

void Watcher::look(const Use& use, bool fresh, bool existing) {
    if (!open()) {
        if (fresh) {
            handlers_.readFailed(RCS_ERROR_UNSUPPORTED, unavailable_);
        }
        return;
    }
    std::optional<uint64_t> count; // What it read, once the system said.
    // A copy that cannot be read is not tried again until the clipboard changes.
    const auto givenUp = [&] {
        if (count) {
            seen_ = *count;
            seenAny_ = true;
        }
        seenOwn_ = false;
    };
    try {
        count = clipboard_->changeCount();
        if (seenAny_ && *count == seen_ && !existing && (!fresh || seenOwn_)) {
            return; // Nothing new; a copy placed here goes nowhere, also after the settings changed.
        }
        Content content = clipboard_->read(use.formats, use.maxBytes);
        seen_ = *count;
        seenAny_ = true;
        seenOwn_ = false;
        failedLooks_ = 0;
        handlers_.copied(std::move(content), existing);
    } catch (const Error& failure) {
        // Another program holds the clipboard: looked at again at the next
        // interval, given up on after a few.
        if (++failedLooks_ < lookAttempts && failure.result() == RCS_ERROR_DEVICE) {
            return;
        }
        failedLooks_ = 0;
        givenUp();
        handlers_.readFailed(failure.result(), failure.what());
    } catch (const std::exception& error) {
        givenUp();
        handlers_.readFailed(RCS_ERROR_DEVICE, error.what());
    }
}

void Watcher::write(const Write& w) {
    if (!open()) {
        if (!w.replacing) {
            handlers_.placed(w.id, RCS_ERROR_UNSUPPORTED, unavailable_);
        }
        return;
    }
    try {
        if (w.replacing && (placed_ != w.id || !seenOwn_ || clipboard_->changeCount() != seen_)) {
            return; // Another copy took its place meanwhile.
        }
        Content content = w.content;
        if (content.files) {
            content.files->placing = w.id;
        }
        seen_ = clipboard_->write(content); // The watch does not take it for a new copy.
        seenAny_ = true;
        seenOwn_ = true;
        placed_ = w.id;
        if (!w.replacing) {
            handlers_.placed(w.id, RCS_OK, {});
        }
    } catch (const Error& failure) {
        if (!w.replacing) {
            handlers_.placed(w.id, failure.result(), failure.what());
        }
    } catch (const std::exception& error) {
        if (!w.replacing) {
            handlers_.placed(w.id, RCS_ERROR_DEVICE, error.what());
        }
    }
}

} // namespace rcsdk::clipboard
