#include "session/cursor_stream.hpp"

#include <algorithm>

namespace rcsdk::session {

void CursorSender::setActive(bool active) {
    std::lock_guard lock(mutex_);
    active_ = active;
    image_.reset(); // Turning on sends the current image and position again.
    sent_.reset();
    send();
}

namespace {

// FNV-1a over everything that makes an image what it is.
uint64_t digest(const CursorImage& image) noexcept {
    uint64_t hash = 0xcbf29ce484222325;
    const auto mix = [&](uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash = (hash ^ ((value >> (8 * i)) & 0xff)) * 0x100000001b3;
        }
    };
    mix(image.type);
    mix(image.width);
    mix(image.height);
    mix(image.hotspotX);
    mix(image.hotspotY);
    for (const uint8_t byte : image.pixels) {
        hash = (hash ^ byte) * 0x100000001b3;
    }
    return hash;
}

} // namespace

uint32_t CursorSender::idOf(const std::shared_ptr<const CursorImage>& image) {
    const auto held = std::find_if(held_.begin(), held_.end(), [&](const auto& entry) {
        return entry.first == image || *entry.first == *image;
    });
    if (held != held_.end()) {
        return held->second;
    }
    const auto [known, added] = ids_.try_emplace(digest(*image), nextId_);
    nextId_ += added;
    const uint32_t id = known->second;
    const auto packed = packPixels(image->pixels);
    for (size_t offset = 0; offset < packed.size(); offset += protocol::maxCursorPart) {
        protocol::CursorShapePart part{id,
                                       image->type,
                                       image->width,
                                       image->height,
                                       image->hotspotX,
                                       image->hotspotY,
                                       uint32_t(packed.size()),
                                       uint32_t(offset),
                                       {}};
        const size_t size = std::min(protocol::maxCursorPart, packed.size() - offset);
        part.bytes.assign(packed.begin() + ptrdiff_t(offset), packed.begin() + ptrdiff_t(offset + size));
        send_(std::move(part));
    }
    held_.emplace_back(image, id);
    if (held_.size() > heldImages) {
        held_.pop_front(); // The viewer evicts the same one (CursorReceiver::part).
    }
    return id;
}

void CursorSender::update(uint32_t stream, const CursorState& state) {
    std::lock_guard lock(mutex_);
    state_ = state;
    stream_ = stream;
    send();
}

void CursorSender::send() {
    const CursorState& state = state_;
    if (!active_ || !state.image) {
        return;
    }
    if (const std::pair image{stream_, state.imageSerial}; image != image_) {
        image_ = image;
        imageId_ = idOf(state.image);
    }
    const protocol::CursorPosition position{state.visible, uint8_t(stream_), state.x, state.y, imageId_};
    if (position != sent_) {
        send_(position);
        sent_ = position;
    }
}

void CursorReceiver::part(const protocol::CursorShapePart& part) {
    // The sharer sends one image's parts after another on the ordered stream.
    if (!pending_.empty() && !pending_.count(part.id)) {
        fail(RCS_ERROR_PROTOCOL, "Invalid message: a cursor shape began before the last one was complete");
    }
    auto& pending = pending_[part.id];
    if (part.offset == 0) {
        pending = {part, {}};
    }
    const auto& header = pending.header;
    if (part.offset != pending.packed.size() || part.packedSize != header.packedSize || part.width != header.width ||
        part.height != header.height) {
        fail(RCS_ERROR_PROTOCOL, "Invalid message: cursor shape parts out of order");
    }
    pending.packed.insert(pending.packed.end(), part.bytes.begin(), part.bytes.end());
    if (pending.packed.size() < header.packedSize) {
        return;
    }
    CursorImage image;
    image.type = header.type;
    image.width = header.width;
    image.height = header.height;
    image.hotspotX = header.hotspotX;
    image.hotspotY = header.hotspotY;
    image.pixels = unpackPixels(pending.packed, size_t(header.width) * header.height);
    pending_.erase(part.id);
    images_.emplace_back(part.id, std::move(image));
    if (images_.size() > CursorSender::heldImages) {
        images_.pop_front(); // As the sharer evicts it.
    }
}

void CursorReceiver::position(const protocol::CursorPosition& position) {
    // The newest copy: an image sent again after eviction may also sit older in the queue.
    const auto image = std::find_if(images_.rbegin(), images_.rend(),
                                    [&](const auto& entry) { return entry.first == position.shapeId; });
    if (image == images_.rend()) {
        fail(RCS_ERROR_PROTOCOL, "Invalid message: cursor position before its shape");
    }
    rcs_cursor_shape shape{};
    const bool fresh = std::find(reported_.begin(), reported_.end(), position.shapeId) == reported_.end();
    if (fresh) {
        reported_.push_back(position.shapeId);
        if (reported_.size() > RCS_MAX_CURSOR_SHAPES) {
            reported_.pop_front(); // As the host drops it.
        }
        const auto& i = image->second;
        shape = {position.shapeId, i.type, i.width, i.height, i.hotspotX, i.hotspotY, i.width * 4, i.pixels.data()};
    }
    session_.cursorChanged(
        {position.visible, position.stream, position.x, position.y, position.shapeId, fresh ? &shape : nullptr});
}

} // namespace rcsdk::session
