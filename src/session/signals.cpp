#include "session/signals.hpp"

namespace rcsdk::session {

SignalQueue::Added SignalQueue::add(const ViewerSignal& signal) {
    Added added;
    if (!seen_.insert(signal.description.ufrag).second) {
        added.repeat = true;
        return added;
    }
    seenOrder_.push_back(signal.description.ufrag);
    if (seenOrder_.size() > remembered) {
        seen_.erase(seenOrder_.front());
        seenOrder_.pop_front();
    }
    if (running_ < maxRunning) {
        ++running_;
        added.start = true;
        return added;
    }
    waiting_.push_back(signal);
    if (waiting_.size() > maxWaiting) {
        added.dropped = std::move(waiting_.front());
        waiting_.pop_front();
    }
    return added;
}

std::optional<ViewerSignal> SignalQueue::finished() {
    if (waiting_.empty()) {
        --running_;
        return std::nullopt;
    }
    auto next = std::move(waiting_.front());
    waiting_.pop_front();
    return next;
}

void Answers::add(transport::ice::Description answer) {
    answers_.push_back(std::move(answer));
    if (answers_.size() > kept) {
        answers_.pop_front();
    }
}

std::optional<transport::ice::Description> Answers::take(const std::string& ufrag) {
    while (!answers_.empty()) {
        auto answer = std::move(answers_.front());
        answers_.pop_front();
        if (answer.answers == ufrag) {
            return answer;
        }
    }
    return std::nullopt;
}

} // namespace rcsdk::session
