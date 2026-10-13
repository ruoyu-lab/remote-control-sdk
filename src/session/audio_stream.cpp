#include "session/audio_stream.hpp"

#include "session/negotiation.hpp"
#include "session/share_agent.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

namespace rcsdk::session {
namespace {

uint64_t durationNs(uint64_t frames) noexcept {
    return frames * 1'000'000'000 / audioRate;
}

// Hands a packet of `format` to the host (RCS_EXT_ENCODED_OUTPUT), if it takes them.
void handOut(Session& session, const AudioFormat& format, const AudioPacket& packet, rcs_viewer_id viewer,
             rcs_audio_direction direction) {
    const auto& output = session.config().encodedOutput;
    if (!output.on_audio) {
        return;
    }
    rcs_encoded_audio view{};
    view.struct_size = sizeof(view);
    view.viewer_id = viewer;
    view.direction = direction;
    view.codec = format.codec;
    view.sample_rate = audioRate;
    view.channels = audioChannels;
    view.frames = packet.frames;
    view.sequence = packet.sequence;
    view.timestamp_ns = packet.timestampNs;
    view.data = packet.data.data();
    view.data_size = uint32_t(packet.data.size());
    view.config = format.config.empty() ? nullptr : format.config.data();
    view.config_size = uint32_t(format.config.size());
    session.callHostWith(output.user_data, output.on_audio, &view);
}

} // namespace

// ---- Share ------------------------------------------------------------------------------

// ---- The capture, shared --------------------------------------------------------------

class AudioHub::Share final : public platform::AudioCapture {
public:
    Share(AudioHub& hub, const Source& source) : hub_(hub), source_(source) {}
    ~Share() override { stop(); }
    void start(const platform::AudioCaptureConfig& config, Deliver deliver, Status status) override {
        hub_.subscribe(*this, {source_, config.excludeCurrentProcess, config.periodFrames}, std::move(deliver),
                       std::move(status));
        started_ = true;
    }
    void stop() noexcept override {
        if (std::exchange(started_, false)) {
            hub_.unsubscribe(*this);
        }
    }
    uint32_t periodFrames() const noexcept override { return hub_.periodFrames(); }

private:
    AudioHub& hub_;
    const Source source_;
    bool started_ = false;
};

AudioHub::~AudioHub() {
    if (capture_) {
        capture_->stop();
    }
}

std::unique_ptr<platform::AudioCapture> AudioHub::capture(const Source& source) {
    return std::make_unique<Share>(*this, source);
}

void AudioHub::subscribe(const Share& share, const Settings& settings, platform::AudioCapture::Deliver deliver,
                         platform::AudioCapture::Status status) {
    std::lock_guard start(captureMutex_);
    if (capture_ && settings == running_) {
        std::lock_guard lock(mutex_);
        if (status_) {
            status(*status_); // As the capture said last.
        }
        subscribers_.push_back({&share, std::move(deliver), std::move(status)});
        return;
    }
    if (capture_) { // Other settings: every sender restarts with them.
        capture_->stop();
        capture_.reset();
    }
    {
        std::lock_guard lock(mutex_);
        subscribers_.push_back({&share, std::move(deliver), std::move(status)});
        status_.reset();
    }
    try {
        const auto& source = settings.source;
        if (source.source == RCS_AUDIO_HOST) {
            capture_ = session_.hostAudio().capture();
        } else if (session_.config().role == RCS_ROLE_VIEW) { // A viewer's microphone: here.
            capture_ = seams::createMicrophoneCapture({source.device, source.voiceProcessing});
        } else { // The sharer's machine, or its microphone: its agent's.
            agent::AudioStart start;
            start.backend = source.backend;
            start.microphone = source.source == RCS_AUDIO_MICROPHONE;
            start.device = source.device;
            start.voiceProcessing = source.voiceProcessing;
            capture_ = std::make_unique<AgentAudioCapture>(session_.shareAgent(), start);
        }
        running_ = settings;
        capture_->start(
            {settings.excludeCurrentProcess, settings.periodFrames},
            [this](std::span<const float> samples, uint64_t timestampNs) { this->deliver(samples, timestampNs); },
            [this](const platform::AudioStatus& status) { this->status(status); });
    } catch (...) {
        capture_.reset();
        std::lock_guard lock(mutex_);
        std::erase_if(subscribers_, [&](const Subscriber& s) { return s.share == &share; });
        throw;
    }
}

void AudioHub::unsubscribe(const Share& share) noexcept {
    std::lock_guard start(captureMutex_);
    bool last;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(subscribers_, [&](const Subscriber& s) { return s.share == &share; });
        last = subscribers_.empty();
    }
    if (last && capture_) {
        capture_->stop(); // No delivery after this.
        capture_.reset();
        std::lock_guard lock(mutex_);
        status_.reset();
    }
}

uint32_t AudioHub::periodFrames() const noexcept {
    std::lock_guard start(captureMutex_);
    return capture_ ? capture_->periodFrames() : 0;
}

void AudioHub::deliver(std::span<const float> samples, uint64_t timestampNs) {
    {
        std::lock_guard lock(mutex_);
        for (const auto& subscriber : subscribers_) {
            subscriber.deliver(samples, timestampNs);
        }
    }
    session_.updateStats([&](Stats& s) { s.audioSend.captured_frames += samples.size() / audioChannels; });
}

void AudioHub::status(const platform::AudioStatus& status) {
    std::lock_guard lock(mutex_);
    status_ = status;
    for (const auto& subscriber : subscribers_) {
        subscriber.status(status);
    }
}

// ---- One viewer's sound ----------------------------------------------------------------

AudioSender::AudioSender(Session& session, Send send, bool lossy, AudioHub& hub, rcs_audio_codec codec,
                         uint32_t bitrate, const rcs_send_audio_config& config, const rcs_audio_tuning_config& tuning,
                         rcs_viewer_id viewer)
    : session_(session), send_(std::move(send)), viewer_(viewer),
      redundant_(tuning.redundancy && lossy && codec != RCS_AUDIO_PCM) {
    try {
        encoder_ = audio::createEncoder(codec, bitrate);
        log::info("audio", std::string(audioCodecName(codec)) + " encoder: " + encoder_->name());
        format_ = AudioFormat{codec, bitrate, encoder_->frames(), encoder_->config()};
        AudioHub::Source source{config.source, RCS_AUDIO_CAPTURE_AUTO, config.device, config.voice_processing != 0};
        if (config.source == RCS_AUDIO_SYSTEM) {
            source.backend =
                tuning.capture_backend ? tuning.capture_backend : session.shareAgent().machine().defaultAudioBackend;
        }
        session.updateStats(viewer_, [&](Stats& s) {
            copyText(s.audioSend.encoder, encoder_->name());
            s.audioSend.codec = codec;
            s.audioSend.bitrate_bps = audio::wireBitrate(codec, bitrate, redundant_);
            s.audioSend.capture_backend = source.backend;
        });
        thread_ = log::thread([this] { run(); }); // Announces the format first.
        capture_ = hub.capture(source);
        capture_->start(
            {config.exclude_current_process != 0, tuning.capture_period_frames},
            [this](std::span<const float> samples, uint64_t timestampNs) { captured(samples, timestampNs); },
            [this](const platform::AudioStatus& status) {
                session_.audioChanged(status, RCS_AUDIO_SENDING, viewer_, format_.codec);
            });
    } catch (const Failure& failure) {
        capture_.reset();
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            arrived_.notify_all();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        encoder_.reset();
        session.audioChanged({false, failure.result(), failure.what()}, RCS_AUDIO_SENDING, viewer_, codec);
    }
}

AudioSender::~AudioSender() {
    if (capture_) {
        capture_->stop(); // No delivery after this.
    }
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        arrived_.notify_all();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void AudioSender::captured(std::span<const float> samples, uint64_t timestampNs) {
    {
        std::lock_guard lock(mutex_);
        if (input_.empty()) {
            inputStartNs_ = timestampNs;
        }
        input_.insert(input_.end(), samples.begin(), samples.end());
        const size_t limit = size_t(maxBacklogMs) * audioRate / 1000 * audioChannels;
        if (input_.size() > limit) { // The sender cannot keep up: the oldest sound goes.
            const size_t excess = input_.size() - limit;
            input_.erase(input_.begin(), input_.begin() + ptrdiff_t(excess));
            inputStartNs_ += durationNs(excess / audioChannels);
            gap_ = true;
            if (!droppedFrames_) {
                log::warning("audio", "Audio could not be sent as fast as it was captured; dropping the oldest");
            }
            droppedFrames_ += excess / audioChannels;
        }
        arrived_.notify_one();
    }
}

void AudioSender::run() noexcept {
    try {
        send_(format_);
    } catch (...) {
        return; // The connection ended: the run reports it.
    }
    std::vector<float> pending; // Taken from input_, waiting to fill a packet.
    uint64_t pendingStartNs = 0;
    for (;;) {
        size_t taken;
        {
            std::unique_lock lock(mutex_);
            arrived_.wait(lock, [&] { return stopping_ || !input_.empty(); });
            if (stopping_) {
                return;
            }
            if (std::exchange(gap_, false)) {
                pending.clear(); // What it would have continued was dropped: start again where sound resumes.
            }
            if (pending.empty()) {
                pendingStartNs = inputStartNs_;
            }
            pending.insert(pending.end(), input_.begin(), input_.end());
            taken = input_.size() / audioChannels;
            input_.clear();
        }
        const uint32_t period = capture_ ? capture_->periodFrames() : 0;
        session_.updateStats(viewer_, [&](Stats& s) {
            if (viewer_) { // A sharer's viewer: what reached its sender. The session's own is the hub's count.
                s.audioSend.captured_frames += taken;
            }
            s.audioSend.capture_period_frames = period;
        });
        try {
            encode(pending, pendingStartNs);
        } catch (const Failure& failure) {
            if (failure.result() == RCS_ERROR_NETWORK || failure.result() == RCS_ERROR_CANCELLED) {
                return; // The connection ended: the run reports it.
            }
            session_.audioChanged({false, failure.result(), std::string("Audio encoder: ") + failure.what()},
                                  RCS_AUDIO_SENDING, viewer_, format_.codec);
            return; // Paused until the sound restarts with new settings or a new connection.
        } catch (const std::exception& exception) {
            session_.audioChanged({false, RCS_ERROR_INTERNAL, exception.what()}, RCS_AUDIO_SENDING, viewer_,
                                  format_.codec);
            return;
        }
    }
}

void AudioSender::encode(std::vector<float>& pending, uint64_t& startNs) {
    size_t offset = 0;
    while (const uint32_t frames = encoder_->next(uint32_t((pending.size() - offset) / audioChannels))) {
        const size_t chunk = size_t(frames) * audioChannels;
        const uint64_t started = monotonicNs();
        auto packets = encoder_->encode(std::span(pending).subspan(offset, chunk));
        const uint64_t encoded = monotonicNs();
        offset += chunk;
        for (auto& data : packets) {
            const size_t size = data.size();
            AudioPacket packet{sequence_++, frames, startNs, std::move(data), 0, {}};
            handOut(session_, format_, packet, viewer_, RCS_AUDIO_SENDING);
            if (redundant_) {
                if (previousFrames_ && size + previous_.size() <= maxAudioPacketBytes) {
                    packet.previousFrames = previousFrames_;
                    packet.previous = previous_;
                }
                previousFrames_ = frames;
                previous_ = packet.data;
            }
            send_(std::move(packet));
            session_.updateStats(viewer_, [&](Stats& s) {
                ++s.audioSend.encoded_packets;
                s.audioSend.encoded_bytes += size;
                s.audioSend.last_encode_ns = encoded - started;
            });
        }
        startNs += durationNs(frames);
    }
    pending.erase(pending.begin(), pending.begin() + ptrdiff_t(offset));
}

// ---- View -------------------------------------------------------------------------------

AudioReceiver::AudioReceiver(Session& session, const AudioFormat& format, const ReceivedSound& settings,
                             rcs_viewer_id viewer)
    : session_(session), format_(format), viewer_(viewer),
      decode_(session.config().role == RCS_ROLE_SHARE || session.config().encodedOutput.decode != 0),
      lastFrames_(format.framesPerPacket) {
    const auto changed = [this](const platform::AudioStatus& status) {
        session_.audioChanged(status, RCS_AUDIO_RECEIVING, viewer_, format_.codec);
    };
    session_.updateStats(viewer_, [&](Stats& s) { s.audioReceive.codec = format.codec; });
    if (!decode_) {
        changed({true, RCS_OK, {}}); // The host takes the packets (RCS_EXT_ENCODED_OUTPUT).
        return;
    }
    try {
        decoder_ = audio::createDecoder(format);
        log::info("audio", std::string(audioCodecName(format.codec)) + " decoder: " + decoder_->name());
        session_.updateStats(viewer_, [&](Stats& s) { copyText(s.audioReceive.decoder, decoder_->name()); });
        const auto& tuning = settings.tuning;
        if (!settings.audio.playback) {
            changed({true, RCS_OK, {}}); // The host plays what on_audio receives.
        } else {
            playout_ = std::make_unique<audio::Playout>(tuning.max_delay_ms, tuning.buffer_margin_us);
            player_ = seams::createAudioPlayer();
            player_->start({tuning.playback_period_frames, tuning.playback_queue_frames},
                           [playout = playout_.get()](float* out, uint32_t frames) { playout->pull(out, frames); },
                           log::bound(changed));
        }
    } catch (const Failure& failure) {
        decoder_.reset();
        player_.reset();
        playout_.reset();
        changed({false, failure.result(), failure.what()});
    }
}

AudioReceiver::~AudioReceiver() {
    if (player_) {
        player_->stop();
    }
    if (playout_) {
        const auto c = playout_->counters();
        log::debug("audio", "Playback: " + std::to_string(c.underruns) + " underruns, " +
                                std::to_string(c.adjustments) + " drift corrections, " +
                                std::to_string(c.droppedFrames) + " frames dropped, " +
                                std::to_string(c.concealedFrames) + " made up");
    }
}

void AudioReceiver::packet(const AudioPacket& packet, uint64_t arrivalNs) {
    uint32_t ahead = 0; // Packets lost before this one.
    if (expected_ && packet.sequence != *expected_) {
        ahead = packet.sequence - *expected_;
        if (ahead >= 0x8000'0000u) {
            return; // Overtaken by later packets: already concealed, or passed on without it.
        }
    }
    // The last of them came along (redundancy). Longer gaps: the buffer primes again instead.
    const bool recovered = ahead && ahead <= 50 && !packet.previous.empty();
    lost_ += ahead - recovered;
    recovered_ += recovered;
    expected_ = packet.sequence + 1;
    handOut(session_, format_, packet, viewer_, RCS_AUDIO_RECEIVING);
    if (!decode_) {
        session_.updateStats(viewer_, [&](Stats& s) {
            ++s.audioReceive.received_packets;
            s.audioReceive.lost_packets = lost_;
            s.audioReceive.recovered_packets = recovered_;
        });
        return;
    }
    if (!decoder_) {
        return;
    }
    decoded_.clear();
    uint64_t concealed = 0, earlier = 0; // Frames made up, and the block's frames before this packet's.
    if (const uint32_t missing = ahead - recovered; missing && ahead <= 50) {
        concealed = uint64_t(missing) * lastFrames_; // As long as the packets around them.
        decoder_->conceal(uint32_t(concealed), decoded_);
        earlier = concealed;
    }
    const auto decode = [&](std::span<const uint8_t> data, uint32_t frames) {
        try {
            decoder_->decode(data, decoded_);
        } catch (const Failure&) {
            ++lost_;
            concealed += frames;
            decoder_->conceal(frames, decoded_);
        }
    };
    if (recovered) {
        decode(packet.previous, packet.previousFrames);
        earlier += packet.previousFrames;
    }
    lastFrames_ = packet.frames;
    decode(packet.data, packet.frames);
    concealedFrames_ += concealed;
    deliver(decoded_, packet.timestampNs - durationNs(earlier), arrivalNs);
}

void AudioReceiver::discontinuity() noexcept {
    if (playout_) {
        playout_->discontinuity();
    }
}

void AudioReceiver::deliver(std::span<const float> samples, uint64_t captureNs, uint64_t arrivalNs) {
    if (samples.empty()) {
        return;
    }
    if (playout_) {
        playout_->push(samples, captureNs, arrivalNs);
    }
    const auto& callbacks = session_.callbacks();
    if (callbacks.on_audio) {
        rcs_audio_frame frame{};
        frame.struct_size = sizeof(frame);
        frame.viewer_id = viewer_;
        frame.sample_rate = audioRate;
        frame.channels = audioChannels;
        frame.frames = uint32_t(samples.size() / audioChannels);
        frame.sequence = ++delivered_;
        frame.timestamp_ns = arrivalNs;
        frame.samples = samples.data();
        session_.callHost(callbacks.on_audio, &frame);
    }
    const uint64_t frames = samples.size() / audioChannels;
    const auto counters = playout_ ? playout_->counters() : audio::Playout::Counters{};
    const auto device = player_ ? player_->device() : platform::AudioPlayer::Device{};
    session_.updateStats(viewer_, [&](Stats& s) {
        auto& r = s.audioReceive;
        ++r.received_packets;
        r.lost_packets = lost_;
        r.recovered_packets = recovered_;
        r.concealed_ms = (concealedFrames_ + counters.concealedFrames) * 1000 / audioRate;
        r.dropped_ms = counters.droppedFrames * 1000 / audioRate;
        r.underruns = counters.underruns;
        r.device_underruns = device.underruns;
        r.played_frames = playout_ ? counters.playedFrames : r.played_frames + frames;
        r.buffer_ms = uint32_t(uint64_t(counters.bufferedFrames) * 1000 / audioRate);
        r.target_ms = uint32_t(uint64_t(counters.targetFrames) * 1000 / audioRate);
        copyText(r.device, device.name);
        r.playback_period_frames = device.periodFrames;
        r.playback_queue_frames = device.queueFrames;
    });
}

// ---- The run's sound ---------------------------------------------------------------------

namespace {

// A bitrate the host gave or the viewer asked for, within what `codec` takes
// on this system (rcs_audio_send_stats.bitrate_bps says which).
uint32_t fittedBitrate(rcs_audio_codec codec, uint32_t bitrate) {
    if (codec == RCS_AUDIO_OPUS) {
        return std::clamp(bitrate, 16'000u, 510'000u);
    }
    if (codec != RCS_AUDIO_AAC) {
        return bitrate; // PCM has its own.
    }
    bitrate = std::clamp(bitrate, 64'000u, 320'000u);
    const auto rates = platform::aacBitrates(); // Only these, where the encoder is picky.
    if (rates.empty()) {
        return bitrate;
    }
    return *std::min_element(rates.begin(), rates.end(), [&](uint32_t a, uint32_t b) {
        return std::abs(int64_t(a) - bitrate) < std::abs(int64_t(b) - bitrate);
    });
}

// Why a direction carries no sound, as the end that `sends` (or receives) it
// says it; the other end is the viewer to a sharer, the sharer to a viewer.
std::string offReason(protocol::AudioOff::Reason reason, bool sends, rcs_role role) {
    const std::string peer = role == RCS_ROLE_SHARE ? "The viewer" : "The sharer";
    switch (reason) {
    case protocol::AudioOff::receiverOff:
        return sends ? peer + " receives no sound" : "This end receives no sound";
    case protocol::AudioOff::noCommonCodec:
        return sends ? peer + " decodes none of this end's audio codecs"
                     : peer + " encodes none of this end's audio codecs";
    default:
        return sends ? "This end sends no sound" : peer + " sends no sound";
    }
}

} // namespace

SoundOut::SoundOut(Session& session, protocol::Connection& connection, AudioHub& hub, rcs_viewer_id viewer,
                   std::function<void(protocol::Message)> post, SoundGroups* groups)
    : session_(session), connection_(connection), hub_(hub), viewer_(viewer), post_(std::move(post)), groups_(groups) {}

SoundPlan SoundOut::plan(const SessionConfig& live, bool receives, const std::vector<rcs_audio_codec>& codecs,
                         const protocol::StreamRequest* request) const {
    const auto& audio = live.sendAudio;
    SoundPlan plan;
    if (!audio.enabled) {
        return plan; // senderOff
    }
    if (!receives) {
        plan.reason = protocol::AudioOff::receiverOff;
        return plan;
    }
    const auto offer = audioOffer(audio.codecs, capabilities().audio_encoders);
    const bool follow = request && live.shareVideo.follow_requests != 0;
    const auto both = [&](rcs_audio_codec codec) {
        return std::find(offer.begin(), offer.end(), codec) != offer.end() &&
               std::find(codecs.begin(), codecs.end(), codec) != codecs.end();
    };
    plan.codec = follow && request->audioCodec && both(request->audioCodec) ? request->audioCodec
                                                                             : negotiatedCodec(offer, codecs);
    if (!plan.codec) {
        plan.reason = protocol::AudioOff::noCommonCodec;
        return plan;
    }
    plan.bitrate = fittedBitrate(plan.codec, follow && request->audioBitrate ? request->audioBitrate : audio.bitrate_bps);
    plan.source = {audio.source, live.audioTuning.capture_backend, audio.device, audio.voice_processing != 0};
    plan.excludeCurrentProcess = audio.exclude_current_process;
    plan.capturePeriodFrames = live.audioTuning.capture_period_frames;
    plan.redundant = live.audioTuning.redundancy && connection_.lossy() && plan.codec != RCS_AUDIO_PCM;
    return plan;
}

void SoundOut::apply(const SessionConfig& live, bool receives, const std::vector<rcs_audio_codec>& codecs,
                     const protocol::StreamRequest* request) {
    const SoundPlan next = plan(live, receives, codecs, request);
    if (next == current_) {
        return;
    }
    const bool wasSending = sender_.has_value() || member_;
    const bool stream = session_.config().role == RCS_ROLE_SHARE; // The sharer's is the stream's sound.
    sender_.reset();
    sound_ = 0;
    member_.reset();
    current_ = next;
    if (!next.codec) {
        if (stream) {
            session_.setAudioStream(0, 0, viewer_);
        }
        post_(protocol::AudioOff{next.reason});
        session_.audioChanged({false, next.reason == protocol::AudioOff::noCommonCodec ? RCS_ERROR_UNSUPPORTED : RCS_OK,
                               offReason(next.reason, true, session_.config().role)},
                              RCS_AUDIO_SENDING, viewer_, 0);
        return;
    }
    if (wasSending) {
        log::info("audio", "Audio restarts with new settings");
    }
    if (stream) {
        session_.setAudioStream(next.codec, audio::wireBitrate(next.codec, next.bitrate), viewer_);
    }
    if (groups_) { // The format end to end, the packets through the forwarder.
        member_ = groups_->join(next, live, [post = post_](const AudioFormat& format) { post(format); });
        sound_ = SoundGroups::number(*member_);
        return;
    }
    // Announces the new format.
    sender_.emplace(
        session_, [&connection = connection_](protocol::Message message) { connection.send(message); },
        connection_.lossy(), hub_, next.codec, next.bitrate, live.sendAudio, live.audioTuning, viewer_);
}

uint32_t SoundOut::wireBitrate() const noexcept {
    return current_ && current_->codec ? audio::wireBitrate(current_->codec, current_->bitrate, current_->redundant)
                                       : 0;
}

// ---- A sharer's sound through a forwarder ----------------------------------------------

class SoundGroups::Group {
public:
    Group(Session& session, AudioHub& hub, const SoundPlan& plan, uint8_t number, const SessionConfig& live,
          const Publish& publish)
        : plan_(plan), number_(number) {
        auto settings = live.sendAudio;
        settings.exclude_current_process = plan.excludeCurrentProcess;
        auto tuning = live.audioTuning;
        tuning.capture_period_frames = plan.capturePeriodFrames;
        tuning.redundancy = plan.redundant;
        sender_.emplace(
            session,
            [this, publish](protocol::Message message) {
                if (const auto* format = std::get_if<AudioFormat>(&message)) {
                    std::lock_guard lock(mutex_);
                    format_ = *format;
                    for (const auto& [id, announce] : members_) {
                        announce(*format_);
                    }
                } else if (const auto* packet = std::get_if<AudioPacket>(&message)) {
                    publish(number_, *packet);
                }
            },
            true, hub, plan.codec, plan.bitrate, settings, tuning, 0);
    }

    const SoundPlan& plan() const noexcept { return plan_; }
    uint8_t number() const noexcept { return number_; }
    uint64_t add(std::function<void(const AudioFormat&)> announce) {
        std::lock_guard lock(mutex_);
        if (format_) {
            announce(*format_);
        }
        members_.emplace(++lastId_, std::move(announce));
        return lastId_;
    }
    void remove(uint64_t id) {
        std::lock_guard lock(mutex_);
        members_.erase(id);
    }

private:
    const SoundPlan plan_;
    const uint8_t number_;
    std::mutex mutex_; // Below.
    std::optional<AudioFormat> format_;
    std::map<uint64_t, std::function<void(const AudioFormat&)>> members_;
    uint64_t lastId_ = 0;
    std::optional<AudioSender> sender_; // Last: it calls the above.
};

SoundGroups::SoundGroups(Session& session, AudioHub& hub, Publish publish)
    : session_(session), hub_(hub), publish_(std::move(publish)) {}

SoundGroups::~SoundGroups() = default;

SoundGroups::Member::~Member() {
    group->remove(id);
}

std::unique_ptr<SoundGroups::Member> SoundGroups::join(const SoundPlan& plan, const SessionConfig& live,
                                                       std::function<void(const AudioFormat&)> announce) {
    std::shared_ptr<Group> group;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(groups_, [](const auto& g) { return g.expired(); });
        std::array<bool, 256> taken{};
        for (const auto& weak : groups_) {
            if (auto running = weak.lock()) {
                taken[running->number()] = true;
                if (running->plan() == plan) {
                    group = std::move(running);
                    break;
                }
            }
        }
        if (!group) {
            const auto free = std::find(taken.begin() + 1, taken.end(), false);
            if (free == taken.end()) {
                fail(RCS_ERROR_BUSY, "255 formats of sound run at once");
            }
            group = std::make_shared<Group>(session_, hub_, plan, uint8_t(free - taken.begin()), live, publish_);
            groups_.push_back(group);
        }
    }
    auto member = std::make_unique<Member>();
    member->id = group->add(std::move(announce));
    member->group = std::move(group);
    return member;
}

uint8_t SoundGroups::number(const Member& member) noexcept {
    return member.group->number();
}

SoundIn::SoundIn(Session& session, const ReceivedSound& settings, rcs_viewer_id viewer)
    : session_(session), viewer_(viewer), offered_(codecMask(settings.audio.codecs)), settings_(settings) {
    thread_ = log::thread([this] { run(); });
}

SoundIn::~SoundIn() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        posted_.notify_all();
    }
    thread_.join();
}

void SoundIn::format(const AudioFormat& format) {
    if (format.codec >= 32 || !(offered_.load() & (1u << format.codec))) {
        fail(RCS_ERROR_PROTOCOL, "The other end sent an audio codec this end did not offer");
    }
    post(format);
}

void SoundIn::packet(const AudioPacket& packet, uint64_t arrivalNs) {
    post(Arrived{packet, arrivalNs});
}

void SoundIn::off(protocol::AudioOff::Reason reason) {
    post(reason);
}

void SoundIn::apply(const ReceivedSound& settings) {
    offered_ |= codecMask(settings.audio.codecs); // Formats of codecs offered before may still be on their way.
    post(settings);
}

void SoundIn::discontinuity() {
    post(Discontinuity{});
}

void SoundIn::post(Command command) {
    std::lock_guard lock(mutex_);
    if (std::holds_alternative<Arrived>(command)) {
        if (queuedPackets_ == maxQueuedPackets) { // The sound's thread is stalled: the oldest packet goes.
            const auto oldest = std::find_if(commands_.begin(), commands_.end(),
                                             [](const Command& c) { return std::holds_alternative<Arrived>(c); });
            commands_.erase(oldest);
            --queuedPackets_;
        }
        ++queuedPackets_;
    }
    commands_.push_back(std::move(command));
    posted_.notify_one();
}

void SoundIn::run() noexcept {
    for (;;) {
        Command command;
        {
            std::unique_lock lock(mutex_);
            posted_.wait(lock, [&] { return stopping_ || !commands_.empty(); });
            if (stopping_) {
                break;
            }
            command = std::move(commands_.front());
            commands_.pop_front();
            queuedPackets_ -= std::holds_alternative<Arrived>(command);
        }
        try {
            handle(command);
        } catch (const std::exception& exception) { // Sound never ends the run.
            receiver_.reset();
            session_.audioChanged({false, RCS_ERROR_INTERNAL, exception.what()}, RCS_AUDIO_RECEIVING, viewer_, 0);
        }
    }
    receiver_.reset(); // The device stops on the thread that opened it.
}

void SoundIn::handle(Command& command) {
    std::visit(Overloaded{
                   [&](const AudioFormat& format) {
                       format_ = format;
                       open();
                   },
                   [&](const Arrived& arrived) {
                       if (receiver_) {
                           receiver_->packet(arrived.packet, arrived.arrivalNs);
                       }
                   },
                   [&](Discontinuity) {
                       if (receiver_) {
                           receiver_->discontinuity();
                       }
                   },
                   [&](protocol::AudioOff::Reason reason) {
                       format_.reset();
                       receiver_.reset();
                       stream(0, 0);
                       if (settings_.audio.enabled) { // Turned off here: reported when that happened.
                           session_.audioChanged({false,
                                                  reason == protocol::AudioOff::noCommonCodec ? RCS_ERROR_UNSUPPORTED
                                                                                              : RCS_OK,
                                                  offReason(reason, false, session_.config().role)},
                                                 RCS_AUDIO_RECEIVING, viewer_, 0);
                       }
                   },
                   [&](const ReceivedSound& settings) {
                       const auto& a = settings.audio;
                       const auto& t = settings.tuning;
                       const auto& o = settings_.tuning;
                       const bool same = a.enabled == settings_.audio.enabled &&
                                         a.playback == settings_.audio.playback && t.max_delay_ms == o.max_delay_ms &&
                                         t.buffer_margin_us == o.buffer_margin_us &&
                                         t.playback_period_frames == o.playback_period_frames &&
                                         t.playback_queue_frames == o.playback_queue_frames;
                       settings_ = settings;
                       if (same) {
                           return;
                       }
                       if (!a.enabled && receiver_) {
                           receiver_.reset();
                           stream(0, 0);
                           session_.audioChanged(
                               {false, RCS_OK, offReason(protocol::AudioOff::receiverOff, false, session_.config().role)},
                               RCS_AUDIO_RECEIVING, viewer_, 0);
                           return;
                       }
                       open(); // New playback settings, or on again (the sharer announces its format once it hears).
                   },
               },
               command);
}

void SoundIn::open() {
    receiver_.reset(); // The old device and decoder go first.
    if (format_ && settings_.audio.enabled) {
        stream(format_->codec, audio::wireBitrate(format_->codec, format_->bitrate));
        receiver_ = std::make_unique<AudioReceiver>(session_, *format_, settings_, viewer_);
    }
}

void SoundIn::stream(rcs_audio_codec codec, uint32_t bitrate) {
    if (session_.config().role == RCS_ROLE_VIEW) { // The sharer's is the stream's sound.
        session_.setAudioStream(codec, bitrate);
    }
}

} // namespace rcsdk::session
