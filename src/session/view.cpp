// View role: receive -> queue -> (the host's packets) -> decode -> deliver,
// plus sending input and this end's settings (the stream it asks for, how it
// wants the pointer, its offer). Decoder and sound follow the host's settings
// while running; with RCS_EXT_ENCODED_OUTPUT's decode 0 there is no decoder.
#include "codecs/codecs.hpp"
#include "protocol/connection.hpp"
#include "session/audio_stream.hpp"
#include "session/connect.hpp"
#include "session/cursor_stream.hpp"
#include "session/data_channel.hpp"
#include "session/host_decoder.hpp"
#include "session/negotiation.hpp"
#include "session/receive_queue.hpp"
#include "session/session.hpp"
#include "util/api.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <optional>

namespace rcsdk::session {
namespace {

using protocol::Connection;

// The first failure of the connection's threads (the reader, the sender, a
// stream's decoding): the run ends with it.
class Failures {
public:
    explicit Failures(Session& session) : session_(session) {}
    void mark(std::exception_ptr failure) noexcept {
        std::lock_guard lock(mutex_);
        if (!first_) {
            first_ = std::move(failure);
        }
        failed_.notify_all();
    }
    // Waits up to `timeout`; rethrows the first failure.
    void check(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        failed_.wait_for(lock, timeout, [&] { return first_ || session_.cancelled(); });
        if (first_) {
            std::rethrow_exception(first_);
        }
    }

private:
    Session& session_;
    std::mutex mutex_;
    std::condition_variable failed_;
    std::exception_ptr first_;
};

// One stream's packets waiting for its decoder, shared by the network reader
// and the stream's decoding thread. Follows the host's rcs_decoder_config
// queue limits, which may change while running.
class PacketQueue {
public:
    PacketQueue(Session& session, uint32_t stream)
        : session_(session), stream_(stream), queue_(session.receiveLimits()) {}

    void push(EncodedPacket packet) {
        bool overflowed;
        std::optional<ReceiveQueue::Resumed> resumed;
        {
            std::lock_guard lock(mutex_);
            queue_.setLimits(session_.receiveLimits());
            queue_.push(std::move(packet), monotonicNs());
            overflowed = publish();
            resumed = queue_.takeResumed();
            ready_.notify_one();
        }
        report(overflowed);
        if (resumed) {
            session_.recovering(stream_, RCS_RECOVERY_LOSS, 0, resumed->method, resumed->frozenNs);
        }
    }

    // The next packet, if one is due within a short wait.
    std::optional<ReceiveQueue::Item> pop() {
        std::optional<ReceiveQueue::Item> item;
        bool overflowed;
        {
            std::unique_lock lock(mutex_);
            ready_.wait_for(lock, 10ms, [&] { return !queue_.empty() || ended_ || session_.cancelled(); });
            queue_.setLimits(session_.receiveLimits());
            item = queue_.pop(monotonicNs());
            overflowed = publish();
        }
        report(overflowed);
        return item;
    }

    // What to ask the sharer for, once after each request of the queue.
    std::optional<ReceiveQueue::Request> takeRequest() {
        std::lock_guard lock(mutex_);
        return queue_.takeRequest();
    }

    // This end stalled (a decoder opening, a slow first picture): see ReceiveQueue::forgiveWaiting.
    void forgiveWaiting() {
        std::lock_guard lock(mutex_);
        queue_.forgiveWaiting(monotonicNs());
    }

    // The decoder was replaced: wait for the next keyframe (reported by the caller).
    void restartAtKeyframe() {
        std::lock_guard lock(mutex_);
        queue_.restartAtKeyframe(monotonicNs());
        publish();
        session_.viewControl().inputs.wake(); // The sender thread asks for a keyframe.
    }

    // The network lost a picture: what follows waits for its recovery (reported once got over).
    void lost() {
        std::lock_guard lock(mutex_);
        queue_.lost(monotonicNs());
        publish();
        session_.viewControl().inputs.wake(); // The sender thread asks for it.
    }

    // The sharer ended the stream: what waits goes, and the decoder closes;
    // a stream that starts again opens one.
    void end() {
        std::lock_guard lock(mutex_);
        queue_.clear();
        ended_ = true;
        publish();
        ready_.notify_one();
    }
    // Once after end(); a packet that comes since starts the stream again.
    bool takeEnded() {
        std::lock_guard lock(mutex_);
        return std::exchange(ended_, false) && queue_.empty();
    }

private:
    // Updates the statistics; true when the queue dropped its backlog (for
    // size or age) since.
    bool publish() {
        session_.updateStats([&](Stats& s) {
            auto& receive = s.video[stream_].receive;
            receive.queued_packets = queue_.size();
            receive.dropped_packets = queue_.dropped();
            receive.recovery_requests = queue_.recoveries();
        });
        if (queue_.recoveries() != requests_) {
            requests_ = queue_.recoveries();
            session_.viewControl().inputs.wake(); // The sender thread asks.
        }
        return std::exchange(overflows_, queue_.overflows()) != queue_.overflows();
    }

    // Outside the lock: the host's event callback must not stall the queue.
    void report(bool overflowed) {
        if (overflowed) {
            session_.recovering(stream_, RCS_RECOVERY_BACKLOG);
        }
    }

    Session& session_;
    const uint32_t stream_;
    std::mutex mutex_;
    std::condition_variable ready_;
    ReceiveQueue queue_;
    uint64_t requests_ = 0, overflows_ = 0; // As publish() last saw them.
    bool ended_ = false;
};

using PacketQueues = std::array<std::unique_ptr<PacketQueue>, RCS_MAX_STREAMS>;

// Reads the sharer's messages on its own thread.
class SharerMessages {
public:
    // `offer`: what this end offered; the sharer may switch among it.
    // `voice`: this end's sound to the sharer, planned with the sharer's
    // `sharerOffer` and again when the sharer sends another. `streamStarts`:
    // a stream is announced, before any of its packets (on this thread).
    SharerMessages(Session& session, Connection& connection, PacketQueues& packets, Failures& failures,
                   const protocol::CodecOffer& offer, SoundIn& audio, SoundOut& voice,
                   const protocol::CodecOffer& sharerOffer, std::function<void(uint32_t)> streamStarts)
        : session_(session), connection_(connection), packets_(packets), failures_(failures),
          streamStarts_(std::move(streamStarts)), audio_(audio), voice_(voice),
          sharerReceives_(sharerOffer.receivesAudio), sharerCodecs_(sharerOffer.receiveAudio), cursor_(session) {
        offered(offer);
        applySound();
        thread_ = log::thread([this] { run(); });
    }
    ~SharerMessages() {
        session_.cancel();
        thread_.join();
    }
    // This end's sound to the sharer, with this end's settings as they are now
    // and the sharer's offer as it last came (either thread).
    void applySound() {
        std::lock_guard lock(soundMutex_);
        voice_.apply(*session_.liveConfig(), sharerReceives_, sharerCodecs_);
    }

    // The sharer's encoding of `stream` this end gets (RCS_EXT_ENCODINGS), as
    // its latest packet says: what keyframe requests and losses name.
    uint32_t encodingOf(uint32_t stream) const noexcept { return encodings_[stream].load(); }

    // This end offers `offer` from now on. Packets of the codecs and formats
    // offered before may still be on their way, so they stay acceptable.
    void offered(const protocol::CodecOffer& offer) noexcept {
        for (size_t i = 0; i < offer.codecs.size() && i < offer.formats.size(); ++i) {
            offered_[offer.codecs[i]] |= offer.formats[i];
        }
    }

private:
    void run() noexcept {
        try {
            while (!session_.cancelled()) {
                handle(connection_.receive());
            }
        } catch (...) {
            if (!session_.cancelled()) {
                failures_.mark(std::current_exception());
            }
        }
    }

    void handle(protocol::Message message) {
        if (auto* packet = std::get_if<EncodedPacket>(&message)) {
            negotiated(packet->codec, packet->picture.format);
            const uint32_t stream = packet->stream;
            const uint64_t bytes = connection_.bytesTransferred();
            session_.updateStats([&](Stats& s) {
                ++s.video[stream].receive.received_packets;
                s.network.bytes = bytes;
                s.network.last_receive_ns = connection_.lastReceiveNs();
                s.network.last_decrypt_ns = connection_.lastDecryptNs();
            });
            if (received_[stream] && packet->encoding != encodings_[stream] && !packet->keyframe) {
                // Another encoding's pictures start at its keyframe, which
                // did not come: the numbers it goes on with are of no use.
                packets_[stream]->restartAtKeyframe();
            }
            received_[stream] = packet->sequence;
            encodings_[stream] = packet->encoding;
            packets_[stream]->push(std::move(*packet));
        } else if (const auto* missing = std::get_if<protocol::VideoMissing>(&message)) {
            if (const auto& received = received_[missing->stream]) {
                // The encoder may leave it out while it still holds this one.
                connection_.send(protocol::RefLost{missing->stream, uint8_t(encodings_[missing->stream].load()),
                                                   *received, false});
            }
        } else if (const auto* gap = std::get_if<protocol::StreamGap>(&message)) {
            packets_[gap->stream]->lost(); // Its dependents cannot decode.
        } else if (const auto* ended = std::get_if<protocol::StreamEnded>(&message)) {
            packets_[ended->stream]->end();
            received_[ended->stream].reset();
            session_.videoStreamEnded(ended->stream);
        } else if (const auto* grant = std::get_if<protocol::ControlGrant>(&message)) {
            auto& control = session_.viewControl();
            if (!grant->allowed || grant->epoch != control.epoch) {
                control.inputs.clear(); // Input queued under the old grant would be discarded anyway.
            }
            control.epoch = grant->epoch;
            control.granted = grant->allowed;
            session_.controlChanged();
            session_.modules().peerControl(0, grant->allowed);
        } else if (const auto* sound = std::get_if<AudioPacket>(&message)) {
            audio_.packet(*sound, monotonicNs());
        } else if (const auto* format = std::get_if<AudioFormat>(&message)) {
            audio_.format(*format);
        } else if (const auto* off = std::get_if<protocol::AudioOff>(&message)) {
            audio_.off(off->reason);
        } else if (const auto* stream = std::get_if<protocol::StreamAnnouncement>(&message)) {
            negotiated(stream->info.codec, stream->info.format);
            streamStarts_(stream->info.stream);
            session_.setVideoStream(stream->info);
        } else if (const auto* offer = std::get_if<protocol::CodecOffer>(&message)) {
            session_.setPeer(peerOf(*offer, connection_.peerKey())); // The sharer's host changed its codecs or sound.
            {
                std::lock_guard lock(soundMutex_);
                sharerReceives_ = offer->receivesAudio;
                sharerCodecs_ = offer->receiveAudio;
            }
            applySound();
        } else if (const auto* part = std::get_if<protocol::DataPart>(&message)) {
            data_.part(*part);
        } else if (const auto* datagram = std::get_if<protocol::DataDatagram>(&message)) {
            data_.datagram(*datagram);
        } else if (const auto* shape = std::get_if<protocol::CursorShapePart>(&message)) {
            cursor_.part(*shape);
        } else if (const auto* position = std::get_if<protocol::CursorPosition>(&message)) {
            cursor_.position(*position);
        } else if (const auto* paused = std::get_if<protocol::SharerPaused>(&message)) {
            session_.peerPaused(paused->paused);
        } else if (const auto* desktop = std::get_if<protocol::DesktopState>(&message)) {
            session_.desktopChanged({desktop->kind, desktop->userLoggedOn});
        } else if (const auto* stalls = std::get_if<protocol::PathStalls>(&message)) {
            session_.peerStalls(*stalls);
        } else if (const auto* refused = std::get_if<protocol::Refused>(&message)) {
            fail(refused->result, "The sharer cannot serve this end: " + refused->reason);
        } else {
            fail(RCS_ERROR_PROTOCOL, "Unexpected message from the sharer");
        }
    }

    void negotiated(rcs_codec codec, rcs_picture_format format) const {
        if (codec >= offered_.size() || !(offered_[codec].load() & (1u << format))) {
            fail(RCS_ERROR_PROTOCOL, "The sharer sent a codec or a picture format this end did not offer");
        }
    }

    Session& session_;
    Connection& connection_;
    PacketQueues& packets_;
    Failures& failures_;
    const std::function<void(uint32_t)> streamStarts_;
    std::array<std::atomic<uint32_t>, 32> offered_{}; // By codec: (1u << rcs_picture_format) bits.
    SoundIn& audio_;
    SoundOut& voice_;
    std::mutex soundMutex_; // Planning voice_, and the sharer's offer below.
    bool sharerReceives_;
    std::vector<rcs_audio_codec> sharerCodecs_;
    CursorReceiver cursor_;
    DataReceiver data_{session_, session_.dataQueue()};
    std::array<std::optional<uint64_t>, RCS_MAX_STREAMS> received_; // Each stream's last picture's sequence...
    std::array<std::atomic<uint32_t>, RCS_MAX_STREAMS> encodings_{}; // ...and encoding.
    std::thread thread_;
};

// This end's settings as the sharer gets them, with what is paused (RCS_PAUSE_* bits).
protocol::ViewerSettings viewerSettings(const SessionConfig& config, uint32_t paused) {
    const auto& r = config.viewRequest;
    return {{r.width, r.height, r.frames_per_second, r.bitrate_bps, r.codec, r.format, r.audio_codec,
             r.audio_bitrate_bps},
            r.cursor,
            r.streams,
            paused};
}

// The sharer's sound as this end takes it: none while it is paused.
ReceivedSound receivedSound(const SessionConfig& config, uint32_t paused) {
    ReceivedSound sound{config.receiveAudio, config.audioTuning};
    if (paused & RCS_PAUSE_AUDIO) {
        sound.audio.enabled = 0;
    }
    return sound;
}

// Sends input, keyframe requests, and this end's changed settings and offer
// on its own thread.
class ViewerSender {
public:
    // `offer` and `settingsSent`: the offer and the settings version runView sent.
    ViewerSender(Session& session, Connection& connection, PacketQueues& packets, Failures& failures,
                 SharerMessages& reader, SoundIn& audio, protocol::CodecOffer offer, uint64_t settingsSent)
        : session_(session), connection_(connection), packets_(packets), failures_(failures), reader_(reader),
          audio_(audio), offer_(std::move(offer)), settingsSent_(settingsSent) {
        thread_ = log::thread([this] { run(); });
    }
    ~ViewerSender() {
        session_.cancel();
        session_.viewControl().inputs.wake();
        thread_.join();
    }

private:
    void run() noexcept {
        auto& inputs = session_.viewControl().inputs;
        uint64_t settingsSent = settingsSent_;
        try {
            while (!session_.cancelled()) {
                const auto item = inputs.pop(50ms, session_.cancelled());
                // The host's own requests (each stream that runs), and each
                // stream's queue's (a lost or late picture).
                const bool host = session_.takeKeyframeRequest();
                for (uint8_t stream = 0; stream < packets_.size(); ++stream) {
                    const auto asked = packets_[stream]->takeRequest();
                    const auto encoding = uint8_t(reader_.encodingOf(stream));
                    if ((host && (!stream || session_.streamInfo(stream).codec)) || (asked && !asked->after)) {
                        connection_.send(protocol::KeyframeRequest{stream, encoding});
                    } else if (asked) {
                        connection_.send(protocol::RefLost{stream, encoding, *asked->after, true});
                    }
                }
                if (const uint64_t version = session_.settingsVersion(); version != settingsSent) {
                    settingsSent = version;
                    const auto live = session_.liveConfig();
                    const uint32_t paused = session_.paused();
                    // Off at once; on once the sharer announces its format.
                    audio_.apply(receivedSound(*live, paused));
                    reader_.applySound(); // This end's own to the sharer.
                    if (auto offer = offerOf(*live); offer != offer_) {
                        offer_ = std::move(offer);
                        reader_.offered(offer_);
                        connection_.send(offer_);
                    }
                    connection_.send(viewerSettings(*live, paused));
                }
                if (item) {
                    const uint64_t started = monotonicNs();
                    connection_.send(protocol::Input{item->event, item->epoch});
                    const uint64_t sent = monotonicNs();
                    session_.updateStats([&](Stats& s) {
                        ++s.input.sent;
                        s.input.last_queue_ns = started - item->enqueuedNs;
                        s.input.last_send_ns = sent - started;
                    });
                }
            }
        } catch (...) {
            if (!session_.cancelled()) {
                failures_.mark(std::current_exception()); // Ends the connection with this reason.
            }
        }
    }

    Session& session_;
    Connection& connection_;
    PacketQueues& packets_;
    Failures& failures_;
    SharerMessages& reader_;
    SoundIn& audio_;
    protocol::CodecOffer offer_; // As the sharer last received it.
    const uint64_t settingsSent_;
    std::thread thread_;
};

// Decodes one stream on its own thread: the host's copy of each packet
// (RCS_EXT_ENCODED_OUTPUT), then the decoder, then the picture to the host.
// The decoder is opened for the stream's codec, picture and size: stream 0's
// at once for the negotiated codec (start-up takes a few hundred
// milliseconds on Windows, while the sharer starts capturing), the others' at
// their first keyframe; a new codec, size or encoding (the sharer switched
// streams or moved this end to another of its encodings, starting with a
// keyframe) or new decoder settings get a new decoder, and a stream that
// ended closes it.
class StreamDecoding {
public:
    StreamDecoding(Session& session, uint32_t stream, PacketQueue& packets, Failures& failures, rcs_codec codec,
                   uint64_t settingsVersion)
        : session_(session), stream_(stream), packets_(packets), failures_(failures),
          opened_{codec, {}, 0, 0, 0, session.liveConfig()->decoder, settingsVersion} {
        if (session.config().encodedOutput.decode && !stream && !(session.paused() & RCS_PAUSE_VIDEO)) {
            decoder_ = openDecoder(); // Here: a decoder that cannot open fails the start.
        }
        thread_ = log::thread([this] { run(); });
    }
    ~StreamDecoding() {
        session_.cancel();
        thread_.join();
    }

private:
    void run() noexcept {
        try {
            decode();
        } catch (...) {
            if (!session_.cancelled()) {
                failures_.mark(std::current_exception());
            }
        }
    }

    // The host's own decoder takes the codecs it handles (fixed for the run).
    std::unique_ptr<platform::VideoDecoder> openDecoder() {
        const auto& host = session_.config().hostDecoder;
        const bool byHost = handles(host, opened_.codec);
        platform::DecoderConfig config;
        config.codec = opened_.codec;
        config.device = platform::retainHostDevice(session_.config().gpuDevice.d3d11_device);
        // HARDWARE_FIRST: the hardware of the host's device's GPU where it takes the stream (on a machine with
        // NVIDIA and Intel GPUs, NVDEC and oneVPL each decode 4:4:4 only on their own).
        config.acceleration = codecs::streamAcceleration(opened_.settings.acceleration, opened_.settings.backend,
                                                         opened_.codec, RCS_CODEC_DECODER, opened_.picture.format, 0,
                                                         0, platform::deviceAdapter(config.device),
                                                         codecImplementations());
        config.backend = opened_.settings.backend;
        config.softwareThreads = session_.config().videoTuning.software_threads;
        config.cpuImages = session_.callbacks().on_frame != nullptr;
        config.picture = opened_.picture;
        auto decoder = byHost ? std::make_unique<HostDecoder>(host, stream_)
                              : codecs::createDecoder(config, codecImplementations());
        decoder->open(config);
        const rcs_acceleration acceleration = config.acceleration;
        // low_latency describes encoders.
        session_.codecChanged({stream_, opened_.codec, byHost ? host.acceleration : acceleration, decoder->backend(),
                               false, false, decoder->name()});
        return decoder;
    }

    void decode() {
        const auto& cancelled = session_.cancelled();
        const rcs_host_decoder_config& host = session_.config().hostDecoder;
        const rcs_encoded_output_config& output = session_.config().encodedOutput;
        auto& decoder = decoder_;
        bool recovering = false;
        // Paused (RCS_PAUSE_VIDEO): packets go unused and no decoder stays
        // open (a backgrounded iOS application may not keep one). The sharer
        // ends the stream here unless it resumed first: then the stream goes
        // on from a keyframe asked for.
        bool paused = session_.paused() & RCS_PAUSE_VIDEO, stale = false;
        while (!cancelled) {
            if (const uint64_t version = session_.settingsVersion(); version != opened_.settingsVersion) {
                opened_.settingsVersion = version;
                const auto next = session_.liveConfig()->decoder;
                const auto& o = opened_.settings;
                if (decoder && (next.acceleration != o.acceleration || next.backend != o.backend)) {
                    log::info("decoder", "New decoder settings");
                    opened_.settings = next;
                    decoder.reset(); // Release the old session before opening another.
                    decoder = openDecoder();
                    packets_.restartAtKeyframe();
                }
                opened_.settings = next;
            }
            if (packets_.takeEnded()) {
                decoder.reset(); // The stream ended; one that starts again opens another.
                opened_.width = opened_.height = 0;
                stale = false;
                continue;
            }
            if (const bool now = session_.paused() & RCS_PAUSE_VIDEO; now != paused) {
                paused = now;
                if (paused) {
                    decoder.reset();
                    opened_.width = opened_.height = 0;
                    stale = true;
                } else if (std::exchange(stale, false)) {
                    packets_.restartAtKeyframe();
                }
            }
            if (!decoder) { // What waits is waiting for one to open (its first keyframe), not late.
                packets_.forgiveWaiting();
            }
            auto item = packets_.pop();
            if (!item || paused) {
                continue;
            }
            // The host's copy first, in decoding order; a packet the queue
            // resumed at (a keyframe, or the recovery of a lost picture)
            // follows dropped or lost ones.
            if (output.on_video) {
                const auto view = publicPacket(item->packet, item->afterGap);
                session_.callHostWith(output.user_data, output.on_video, &view);
            }
            if (!output.decode) {
                continue; // decode 0: the packets are all the host gets.
            }
            bool reopened = false;
            if (const auto& p = item->packet; p.keyframe) {
                const bool resized = opened_.width && (p.width != opened_.width || p.height != opened_.height);
                if (!decoder || p.codec != opened_.codec || p.picture != opened_.picture || resized ||
                    p.encoding != opened_.encoding) {
                    log::info("decoder", "Stream " + std::to_string(stream_) + " is " + codecName(p.codec) + " " +
                                             pictureText(p.picture) + " " + std::to_string(p.width) + "x" +
                                             std::to_string(p.height) + " now");
                    opened_.codec = p.codec;
                    opened_.picture = p.picture;
                    opened_.encoding = p.encoding;
                    decoder.reset(); // Release the old session before opening another.
                    decoder = openDecoder();
                    reopened = true;
                }
                opened_.width = p.width;
                opened_.height = p.height;
            }
            if (!decoder) {
                continue; // A stream starts at a keyframe.
            }
            if (item->resetDecoder && !reopened) {
                decoder->reset();
                session_.updateStats([&](Stats& s) { ++s.video[stream_].receive.decoder_resets; });
            }
            const uint64_t started = monotonicNs();
            // A decoder just opened, or a step (decoding and handing over)
            // that took more than half the age limit: a stall of this end's
            // own (a codec opened on its first packet, the first picture's
            // conversion), and what arrived meanwhile was not late. Slowness
            // on every packet still ages the queue.
            const auto forgiveStall = [&] {
                if (const uint32_t maxAge = session_.receiveLimits().maxAgeMs;
                    reopened || (maxAge && monotonicNs() - started > uint64_t(maxAge) * 500'000)) {
                    packets_.forgiveWaiting();
                }
            };
            VideoFrame frame;
            bool pictureReady;
            try {
                pictureReady = decoder->decode(item->packet, frame, cancelled);
            } catch (const Failure& failure) {
                // Decoders can fail transiently: sleep or a GPU reset
                // invalidates their session. Recover once with a new decoder
                // from the next keyframe; failing again before a picture
                // decodes is final.
                if (recovering || failure.result() == RCS_ERROR_CANCELLED) {
                    throw;
                }
                log::warning("decoder", std::string("Decoder error: ") + failure.what());
                recovering = true;
                decoder.reset(); // Release the failed session before opening another.
                session_.recovering(stream_, RCS_RECOVERY_DECODER);
                decoder = openDecoder();
                packets_.restartAtKeyframe();
                session_.updateStats([&](Stats& s) { ++s.video[stream_].receive.decoder_recoveries; });
                continue;
            }
            if (!pictureReady) {
                forgiveStall();
                continue;
            }
            recovering = false;
            const uint64_t decoded = monotonicNs();
            session_.updateStats([&](Stats& s) {
                auto& receive = s.video[stream_].receive;
                ++receive.decoded_frames;
                receive.last_decode_ns = decoded - started;
                receive.last_queue_ns = started - item->arrivalNs;
                receive.last_pipeline_ns = decoded - item->arrivalNs;
            });
            if (handles(host, opened_.codec)) {
                forgiveStall();
                continue; // The host's decoder took it: the picture is the host's.
            }
            frame.stream = stream_;
            frame.sequence = item->packet.sequence;
            frame.timestampNs = item->arrivalNs;
            session_.deliver(frame);
            forgiveStall();
        }
    }

    Session& session_;
    const uint32_t stream_;
    PacketQueue& packets_;
    Failures& failures_;
    // The stream and decoder settings the decoder was opened for.
    struct Opened {
        rcs_codec codec;
        Picture picture;
        uint32_t encoding = 0;
        uint32_t width = 0, height = 0;
        rcs_decoder_config settings;
        uint64_t settingsVersion;
    } opened_;
    std::unique_ptr<platform::VideoDecoder> decoder_;
    std::thread thread_; // Last.
};

} // namespace

void runView(Session& session) {
    CancelToken cancelled = session.cancelled();

    if (!usesIce(session.config().connect.transport)) { // P2P and relay connect once they gathered.
        session.setState(RCS_STATE_CONNECTING);
    }
    NetworkSeen networkSeen = networkSeenNow(session); // What the connection's sockets are opened with.
    const auto connection = connectToSharer(session);
    const LeaveNotice leave(session, *connection);
    session.connectionEstablished();
    log::info("network", "Connected");
    // The offer and settings sent here, and the decoder and sound they open,
    // are those of this copy; later changes are picked up while running.
    const uint64_t settingsSent = session.settingsVersion();
    const LiveConfig settings = session.liveConfig();
    const SessionConfig& live = *settings;
    const auto offer = offerOf(live);
    const auto sharerOffer = exchangeOffers(
        *connection, offer, viewerSettings(live, session.paused()),
        {[&](uint32_t timeoutMs) { session.admission(RCS_ADMISSION_WAITING, 0, timeoutMs); },
         [&] { session.admission(RCS_ADMISSION_ADMITTED, 0); },
         [&](rcs_result result, const std::string& reason) {
             session.admission(RCS_ADMISSION_REFUSED, 0, 0, result, reason);
         }});
    session.setPeer(peerOf(sharerOffer, connection->peerKey()));
    // This end's modules go first (the sharer answers with its own), and none
    // is active with the sharer once the connection ended: after its threads.
    struct ModulePeer {
        explicit ModulePeer(Session& s) : session(s) { session.modules().peerConnected(0); }
        ~ModulePeer() { session.modules().peerGone(0); }
        Session& session;
    } modulePeer{session};
    session.modules().listTo(0, session.dataQueue());
    const rcs_codec codec = negotiatedCodec(sharerOffer, offer);
    if (!codec) {
        fail(RCS_ERROR_UNSUPPORTED, negotiatedCodec(sharerOffer.codecs, offer.codecs)
                                        ? formatMismatch(sharerOffer, offer)
                                        : "The sharer cannot encode " + codecList(offer.codecs) + "; it offers " +
                                              codecList(sharerOffer.codecs));
    }
    log::info("session", std::string("Negotiated ") + codecName(codec) + " (this end offers " +
                             codecList(offer.codecs) + ", the sharer " + codecList(sharerOffer.codecs) + ")");
    if (offer.receivesAudio && sharerOffer.sendsAudio && !negotiatedCodec(sharerOffer.sendAudio, offer.receiveAudio)) {
        fail(RCS_ERROR_UNSUPPORTED, "The sharer cannot encode " + audioCodecList(offer.receiveAudio) +
                                        " audio; it offers " + audioCodecList(sharerOffer.sendAudio));
    }
    if (offer.sendsAudio && sharerOffer.receivesAudio && !negotiatedCodec(offer.sendAudio, sharerOffer.receiveAudio)) {
        fail(RCS_ERROR_UNSUPPORTED, "The sharer cannot decode " + audioCodecList(offer.sendAudio) +
                                        " audio; it offers " + audioCodecList(sharerOffer.receiveAudio));
    }
    // RUNNING before the connection's other events (codec, stream, sound,
    // control): the host can act on each as it comes.
    session.setState(RCS_STATE_RUNNING);
    connection->setMigrate(live.paths.migrate != 0); // Kept from now on when its path goes (rcs_paths_config).
    SoundIn sound(session, receivedSound(live, session.paused()), 0);
    // This end's own sound to the sharer (its microphone, or its host's).
    AudioHub hub(session);
    SoundOut voice(session, *connection, hub, 0, [&connection](protocol::Message message) {
        connection->send(message);
    });
    Failures failures(session);
    PacketQueues packets;
    for (uint32_t stream = 0; stream < packets.size(); ++stream) {
        packets[stream] = std::make_unique<PacketQueue>(session, stream);
    }
    // Decoding first (stream 0's decoder opens now; early packets wait in the
    // socket rather than ageing in the receive queue; another stream's
    // thread starts when the reader hears of it), then the reader and the
    // sender; destroyed the other way round.
    std::array<std::unique_ptr<StreamDecoding>, RCS_MAX_STREAMS> decoding;
    decoding[0] = std::make_unique<StreamDecoding>(session, 0, *packets[0], failures, codec, settingsSent);
    const auto streamStarts = [&](uint32_t stream) {
        if (!decoding[stream]) {
            decoding[stream] =
                std::make_unique<StreamDecoding>(session, stream, *packets[stream], failures, codec, settingsSent);
        }
    };
    SharerMessages reader(session, *connection, packets, failures, offer, sound, voice, sharerOffer, streamStarts);
    ViewerSender sender(session, *connection, packets, failures, reader, sound, offer, settingsSent);
    DataSender data(session, *connection, session.dataQueue(), cancelled, [&session] { session.cancel(); });

    // A path that stalls or is lost is looked for anew: P2P and RELAY through
    // the host's signalling, TCP over a TCP connection of its own.
    std::unique_ptr<PathFinder> finder;
    if (usesIce(live.connect.transport)) {
        finder = std::make_unique<PathRestart>(session, static_cast<protocol::UdpConnection&>(*connection), 0);
    } else if (live.connect.transport == RCS_TRANSPORT_TCP) {
        finder = std::make_unique<TcpRedial>(session, static_cast<protocol::UdpConnection&>(*connection));
    }
    auto nextPathCheck = Clock::now();
    uint64_t pathSettings = settingsSent;
    while (!cancelled) {
        if (const auto now = Clock::now(); now >= nextPathCheck) {
            if (const auto said = session.publishPath(connection->status())) {
                connection->send(*said);
            }
            nextPathCheck = now + 250ms;
        }
        if (const uint64_t version = session.settingsVersion(); version != pathSettings) {
            pathSettings = version;
            connection->setMigrate(session.liveConfig()->paths.migrate != 0);
        }
        followNetworkChoice(session, *connection, finder.get(), networkSeen);
        for (const auto& event : connection->takePathEvents()) {
            session.pathChanged(event);
            if (finder) {
                finder->pathChanged(event);
            }
            if (event.change == RCS_PATH_LOST) {
                session.pathLost(Clock::now() - std::chrono::milliseconds(live.networkTuning.idle_timeout_ms));
            } else if (event.change == RCS_PATH_CHANGED) {
                if (session.state() == RCS_STATE_RECONNECTING) {
                    session.pathBack();
                }
                sound.discontinuity();
            }
        }
        failures.check(50ms);
    }
}

} // namespace rcsdk::session
