#include "machine_half.hpp"

namespace rcsdk::clipboard {

Machine::Machine(rcs_module module, rcs_role role)
    : module_(module), role_(role), pastes_(std::make_shared<Pastes>([module](const ToSession& message) {
          const Bytes bytes = encode(message);
          rcs_module_send_machine(module, bytes.data(), uint32_t(bytes.size()));
      })) {}

void Machine::tell(const ToSession& message) {
    const Bytes bytes = encode(message);
    rcs_module_send_machine(module_, bytes.data(), uint32_t(bytes.size()));
}

void Machine::stop() {
    pastes_->stop(); // Pastes waiting for files hear they will not come.
    watcher_.reset();
}

void Machine::fromSession(std::span<const uint8_t> data) {
    if (!told_) {
        std::string why;
        const uint32_t files = systemFiles(why);
        tell(AbilitiesMessage{files, why});
        told_ = true;
    }
    ToMachine message;
    try {
        message = decodeToMachine(data);
    } catch (const Error& error) {
        rcs_module_log(module_, RCS_LOG_ERROR,
                       (std::string("The session half sent what this end does not read: ") + error.what()).c_str());
        return;
    }
    if (const auto* use = std::get_if<UseMessage>(&message)) {
        if (!use->use) {
            watcher_.reset(); // Joined here: its handlers only send.
            return;
        }
        pastes_->configure(use->use->filesDirectory, use->use->standIn);
        if (watcher_) {
            watcher_->use(*use->use);
            return;
        }
        Watcher::Handlers handlers;
        handlers.copied = [this](Content content, bool existing) {
            tell(CopiedMessage{existing, std::move(content)});
        };
        handlers.readFailed = [this](rcs_result result, const std::string& why) {
            tell(ReadFailedMessage{result, why});
        };
        handlers.placed = [this](uint64_t id, rcs_result result, const std::string& why) {
            tell(PlacedMessage{id, result, why});
        };
        watcher_ = std::make_unique<Watcher>(role_, *use->use, std::move(handlers), pastes_);
    } else if (auto* place = std::get_if<PlaceMessage>(&message)) {
        this->place(std::move(*place));
    } else if (auto* listed = std::get_if<ListedMessage>(&message)) {
        pastes_->listed(std::move(*listed));
    } else if (auto* answer = std::get_if<DataMessage>(&message)) {
        pastes_->data(std::move(*answer));
    } else if (auto* ended = std::get_if<EndedMessage>(&message)) {
        if (const auto ahead = aheads_.find(ended->request); ahead != aheads_.end()) {
            // All of them came: placed again as themselves, where they are.
            if (ended->result == RCS_OK && ended->complete && watcher_) {
                Content whole = ahead->second.content;
                for (auto& entry : whole.files->entries) {
                    entry.path = ahead->second.directory + "/" + entry.name;
                }
                watcher_->replace(ahead->second.placing, std::move(whole));
            }
            aheads_.erase(ahead);
        }
        pastes_->ended(std::move(*ended));
    } else {
        pastes_->revoke(std::get<RevokedMessage>(message).placing);
    }
}

void Machine::place(PlaceMessage place) {
    if (!watcher_) {
        tell(PlacedMessage{place.id, RCS_ERROR_INVALID_STATE, "This machine's clipboard is not used now"});
        return;
    }
    if (place.ahead && place.content.files) {
        try {
            const std::string directory = pastes_->directoryFor(place.content.files->publication);
            const auto fetch = pastes_->fetch(place.id, RCS_FILE_FETCH_INTO, directory, true);
            aheads_[fetch->id] = {place.id, place.content, directory};
        } catch (const Error& error) {
            rcs_module_log(module_, RCS_LOG_WARNING,
                           (std::string("The copy's files are not fetched ahead: ") + error.what()).c_str());
        }
    }
    watcher_->place(place.id, std::move(place.content));
}

} // namespace rcsdk::clipboard
