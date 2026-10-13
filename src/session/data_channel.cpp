#include "session/data_channel.hpp"

#include "session/session.hpp"
#include "util/log.hpp"

#include <rcsdk/module.h>

#include <utility>

namespace rcsdk::session {

using protocol::DataPart;

DataQueue::Queue& DataQueue::queue(uint8_t module) {
    if (module >= queues_.size()) {
        queues_.resize(module + 1u);
    }
    return queues_[module];
}

DataQueue::Item* DataQueue::replaced(Queue& q, const Item& item) noexcept {
    if (item.replace) {
        for (auto waiting = q.items.rbegin(); waiting != q.items.rend(); ++waiting) {
            if (waiting->replace) {
                return &*waiting;
            }
        }
    }
    return nullptr;
}

DataQueue::Push DataQueue::push(Item item, size_t limit) {
    std::lock_guard lock(mutex_);
    if (!open_) {
        return Push::closed;
    }
    auto& q = queue(item.module);
    Item* old = replaced(q, item);
    const size_t freed = old ? old->bytes.size() : 0;
    if (q.bytes - freed + item.bytes.size() > limit) {
        q.refused = true;
        return Push::full;
    }
    q.bytes += item.bytes.size() - freed;
    if (old) {
        *old = std::move(item);
    } else {
        q.items.push_back(std::move(item));
    }
    arrived_.notify_one();
    return Push::queued;
}

DataQueue::Push DataQueue::fits(const Item& item, size_t limit) {
    std::lock_guard lock(mutex_);
    if (!open_) {
        return Push::closed;
    }
    auto& q = queue(item.module);
    const Item* old = replaced(q, item);
    if (q.bytes - (old ? old->bytes.size() : 0) + item.bytes.size() > limit) {
        q.refused = true;
        return Push::full;
    }
    return Push::queued;
}

DataQueue::Push DataQueue::room(size_t bytes, size_t limit) {
    std::lock_guard lock(mutex_);
    if (!open_) {
        return Push::closed;
    }
    auto& q = queue(0);
    if (q.bytes + bytes > limit) {
        q.refused = true;
        return Push::full;
    }
    return Push::queued;
}

bool DataQueue::pushFirst(Item item) {
    std::lock_guard lock(mutex_);
    if (!open_) {
        return false;
    }
    if (item.kind == DataPart::moduleHello) {
        for (auto& waiting : first_) {
            if (waiting.kind == DataPart::moduleHello && waiting.module == item.module) {
                waiting = std::move(item);
                return true;
            }
        }
    }
    first_.push_back(std::move(item));
    arrived_.notify_one();
    return true;
}

void DataQueue::open() {
    std::lock_guard lock(mutex_);
    open_ = true;
}

std::optional<DataQueue::Item> DataQueue::pop(std::chrono::milliseconds timeout, CancelToken cancelled) {
    std::unique_lock lock(mutex_);
    const auto waiting = [&] {
        return !first_.empty() ||
               std::any_of(queues_.begin(), queues_.end(), [](const Queue& q) { return !q.items.empty(); });
    };
    arrived_.wait_for(lock, timeout, [&] { return waiting() || cancelled.load(); });
    if (!first_.empty()) {
        Item item = std::move(first_.front());
        first_.pop_front();
        return item;
    }
    for (size_t i = 0; i < queues_.size(); ++i) {
        const size_t next = (turn_ + i) % queues_.size();
        auto& q = queues_[next];
        if (q.items.empty()) {
            continue;
        }
        Item item = std::move(q.items.front());
        q.items.pop_front();
        q.bytes -= item.bytes.size();
        turn_ = next + 1;
        return item;
    }
    return std::nullopt;
}

bool DataQueue::roomAgain(uint8_t module, size_t limit) {
    std::lock_guard lock(mutex_);
    auto& q = queue(module);
    if (!q.refused || q.bytes > limit / 2) {
        return false;
    }
    q.refused = false;
    return true;
}

bool DataQueue::clear() {
    std::lock_guard lock(mutex_);
    open_ = false;
    const bool refused = !queues_.empty() && queues_[0].refused;
    queues_.clear();
    first_.clear();
    turn_ = 0;
    return refused;
}

size_t DataQueue::bytes(uint8_t module) const {
    std::lock_guard lock(mutex_);
    return module < queues_.size() ? queues_[module].bytes : 0;
}

DataSender::DataSender(Session& session, protocol::Connection& connection, DataQueue& queue, CancelToken cancelled,
                       std::function<void()> cancel, rcs_viewer_id viewer)
    : session_(session), connection_(connection), queue_(queue), cancelled_(cancelled), cancel_(std::move(cancel)),
      viewer_(viewer) {
    thread_ = log::thread([this] { run(); });
}

DataSender::~DataSender() {
    cancel_();
    thread_.join();
}

void DataSender::run() noexcept {
    auto& queue = queue_;
    auto& modules = session_.modules();
    try {
        while (!cancelled_) {
            auto item = queue.pop(50ms, cancelled_);
            if (!item) {
                continue;
            }
            const rcs_network_tuning_config settings = session_.liveConfig()->networkTuning;
            const size_t size = item->bytes.size();
            const uint64_t tag = item->tag;
            if (item->unreliable) {
                connection_.send(protocol::DataDatagram{item->module, item->channel, std::move(item->bytes)});
            } else {
                sendReliable(*item, settings.in_flight_bytes);
            }
            if (item->kind == DataPart::data) {
                session_.updateStats(viewer_, [&](Stats& s) {
                    ++s.data.sent_messages;
                    s.data.sent_bytes += size;
                });
                if (queue.roomAgain(0, settings.send_queue_bytes)) {
                    session_.dataRoom(queue.bytes(), viewer_);
                }
            } else if (item->kind == DataPart::moduleMessage) {
                modules.sent(viewer_, item->module, tag, item->unreliable, size);
                if (queue.roomAgain(item->module, modules.queueLimit(item->module))) {
                    modules.room(viewer_, item->module);
                }
            }
        }
    } catch (...) {
        // The connection failed: its reader reports it.
    }
}

// Input sent meanwhile waits behind at most `inFlight` bytes.
void DataSender::waitForRoom(size_t inFlight) {
    while (connection_.unackedBytes() > inFlight) {
        throwIfCancelled(cancelled_);
        connection_.waitForUnackedBelow(inFlight, 20ms);
    }
}

void DataSender::sendReliable(const DataQueue::Item& item, size_t inFlight) {
    const size_t partSize = connection_.maxDataPartBytes();
    const size_t count = std::max<size_t>(1, (item.bytes.size() + partSize - 1) / partSize); // An empty hello: one.
    for (size_t index = 0; index < count; ++index) {
        waitForRoom(inFlight);
        const size_t offset = index * partSize;
        const auto begin = item.bytes.begin() + ptrdiff_t(offset);
        const auto end = begin + ptrdiff_t(std::min(partSize, item.bytes.size() - offset));
        connection_.send(DataPart{item.kind, item.module, item.channel, uint32_t(index), uint32_t(count),
                                  crypto::Bytes(begin, end)});
    }
}

void DataReceiver::part(const DataPart& part) {
    auto& modules = session_.modules();
    if (part.index == 0) {
        if (count_) { // Messages come whole, one after another.
            fail(RCS_ERROR_PROTOCOL, "A data message began before the one before it was complete");
        }
        kind_ = part.kind;
        module_ = part.module;
        channel_ = part.channel;
        count_ = part.count;
        next_ = 0;
        bytes_.clear();
        switch (kind_) {
        case DataPart::data:
            limit_ = RCS_DATA_MAX_MESSAGE;
            break;
        case DataPart::moduleMessage:
            limit_ = modules.maxMessage(viewer_, module_); // The peer broke the protocol: its connection fails.
            break;
        case DataPart::moduleList:
            limit_ = protocol::maxModuleListBytes;
            break;
        case DataPart::moduleHello:
            limit_ = RCS_MODULE_HELLO_MAX;
            break;
        }
    }
    if (part.index != next_ || part.kind != kind_ || part.module != module_ || part.channel != channel_ ||
        part.count != count_ || bytes_.size() + part.bytes.size() > limit_) {
        fail(RCS_ERROR_PROTOCOL, "Data message parts out of order, or too many");
    }
    bytes_.insert(bytes_.end(), part.bytes.begin(), part.bytes.end());
    if (++next_ < count_) {
        return;
    }
    count_ = next_ = 0;
    const crypto::Bytes message = std::move(bytes_);
    bytes_.clear();
    switch (kind_) {
    case DataPart::data:
        session_.deliverData(channel_, 0, message, viewer_);
        break;
    case DataPart::moduleMessage:
        modules.received(viewer_, module_, message, false);
        break;
    case DataPart::moduleList:
        modules.peerList(viewer_, protocol::decodeModuleList(message), queue_);
        break;
    case DataPart::moduleHello:
        modules.peerHello(viewer_, module_, message);
        break;
    }
}

void DataReceiver::datagram(const protocol::DataDatagram& datagram) {
    if (!datagram.module) {
        session_.deliverData(datagram.channel, RCS_DATA_UNRELIABLE, datagram.bytes, viewer_);
        return;
    }
    // One may overtake the sender's module list: the module is not active yet, and it is dropped.
    session_.modules().received(viewer_, datagram.module, datagram.bytes, true);
}

} // namespace rcsdk::session
