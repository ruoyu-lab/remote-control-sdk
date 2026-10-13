// iOS only, in the simulator: the application's audio session as the
// coordinator sets it up for playback (RCS_AUDIO_SESSION_SDK) or leaves it to
// the host (HOST), a player through the system's interruptions, the
// application's state from UIKit's notifications, the pasteboard as the
// clipboard module reads and writes it, and the power module's requests.
#include "pastes.hpp"
#include "platform/platform.hpp"
#include "power_request.hpp"
#include "seams/seams.hpp"
#include "support/check.hpp"
#include "system.hpp"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace rcsdk;
using namespace std::chrono_literals;

namespace {

template <class Condition>
bool waitFor(Condition condition, std::chrono::milliseconds limit = 5s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// A player's reports, and the frames it was asked for.
struct Playing {
    std::mutex mutex;
    platform::AudioStatus status{false, RCS_OK, {}};
    int reports = 0;
    std::atomic<uint64_t> frames{0};

    std::unique_ptr<platform::AudioPlayer> start() {
        auto player = platform::createAudioPlayer();
        player->start(
            {},
            [this](float* samples, uint32_t count) {
                std::fill(samples, samples + size_t(count) * 2, 0.0f);
                frames += count;
            },
            [this](const platform::AudioStatus& now) {
                std::lock_guard lock(mutex);
                status = now;
                ++reports;
            });
        return player;
    }
    bool active() {
        std::lock_guard lock(mutex);
        return status.active;
    }
    std::string message() {
        std::lock_guard lock(mutex);
        return status.message;
    }
};

void postInterruption(AVAudioSessionInterruptionType type) {
    [NSNotificationCenter.defaultCenter postNotificationName:AVAudioSessionInterruptionNotification
                                                      object:AVAudioSession.sharedInstance
                                                    userInfo:@{AVAudioSessionInterruptionTypeKey : @(type)}];
}

void audioSession() {
    CHECK(platform::audioSessionConfig().manager == RCS_AUDIO_SESSION_SDK);
    Playing first;
    auto player = first.start();
    CHECK(waitFor([&] { return first.active(); }));
    CHECK(waitFor([&] { return first.frames > 4800; })); // It plays (silence).
    CHECK([AVAudioSession.sharedInstance.category isEqualToString:AVAudioSessionCategoryPlayback]);
    CHECK(!(AVAudioSession.sharedInstance.categoryOptions & AVAudioSessionCategoryOptionMixWithOthers));
    CHECK(player->device().periodFrames > 0);

    // Mixing with others applies to what plays, as soon as the engine has set up again.
    rcs_audio_session_config config = RCS_AUDIO_SESSION_CONFIG_INIT;
    config.mix_with_others = 1;
    platform::setAudioSessionConfig(config);
    CHECK(waitFor([] { return AVAudioSession.sharedInstance.categoryOptions & AVAudioSessionCategoryOptionMixWithOthers; }));

    // A second player shares the session; an interruption pauses both, with why, and its end resumes them.
    Playing second;
    auto other = second.start();
    CHECK(waitFor([&] { return second.active(); }));
    postInterruption(AVAudioSessionInterruptionTypeBegan);
    CHECK(waitFor([&] { return !first.active() && !second.active(); }));
    CHECK(first.message().find("interrupted") != std::string::npos);
    postInterruption(AVAudioSessionInterruptionTypeEnded);
    CHECK(waitFor([&] { return first.active() && second.active(); }));
    const uint64_t played = first.frames;
    CHECK(waitFor([&] { return first.frames > played + 4800; }));
    other->stop();
    player->stop();

    // HOST: the SDK sets nothing; a category that cannot play is reported, not changed.
    config = RCS_AUDIO_SESSION_CONFIG_INIT;
    config.manager = RCS_AUDIO_SESSION_HOST;
    platform::setAudioSessionConfig(config);
    CHECK([AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryRecord error:nil]);
    Playing host;
    auto refused = host.start();
    CHECK(waitFor([&] { return host.reports > 0; }));
    CHECK(!host.active() && host.message().find("Record") != std::string::npos);
    CHECK([AVAudioSession.sharedInstance.category isEqualToString:AVAudioSessionCategoryRecord]);
    refused->stop();
    CHECK([AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryAmbient error:nil]);
    Playing allowed;
    auto playing = allowed.start();
    CHECK(waitFor([&] { return allowed.active(); }));
    CHECK([AVAudioSession.sharedInstance.category isEqualToString:AVAudioSessionCategoryAmbient]);
    playing->stop();
    platform::setAudioSessionConfig(RCS_AUDIO_SESSION_CONFIG_INIT);
}

// A microphone on the engine beside a player: 48 kHz stereo comes, the
// session records and plays (voice chat with voice processing asked for),
// and the player goes on; without the microphone it plays only again. Needs
// the permission: xcrun simctl privacy booted grant microphone
// local.rcsdk.rcsdk_test_ios_platform (skipped without it).
void microphone() {
    if (platform::microphonePermission() != RCS_PERMISSION_GRANTED) {
        std::printf("No microphone permission: the microphone is not tested\n");
        return;
    }
    Playing playing;
    auto player = playing.start();
    CHECK(waitFor([&] { return playing.active(); }));
    Playing recording; // Its status; the sound counted below.
    std::atomic<uint64_t> frames{0};
    std::atomic<bool> processing{false};
    auto capture = platform::createMicrophoneCapture({0, true});
    capture->start(
        {}, [&](std::span<const float> samples, uint64_t) { frames += samples.size() / 2; },
        [&](const platform::AudioStatus& status) {
            processing = status.voiceProcessing;
            std::lock_guard lock(recording.mutex);
            recording.status = status;
            ++recording.reports;
        });
    CHECK(waitFor([&] { return recording.reports > 0; }));
    std::printf("microphone: %s%s\n", recording.active() ? "active" : recording.message().c_str(),
                processing ? ", voice processing" : "");
    CHECK(recording.active());
    CHECK(waitFor([&] { return frames > 24000; })); // Half a second.
    CHECK(capture->periodFrames() > 0);
    AVAudioSession* session = AVAudioSession.sharedInstance;
    CHECK([session.category isEqualToString:AVAudioSessionCategoryPlayAndRecord] &&
          [session.mode isEqualToString:AVAudioSessionModeVoiceChat]);
    CHECK(session.categoryOptions & AVAudioSessionCategoryOptionDefaultToSpeaker);
    CHECK(waitFor([&] { return playing.active(); })); // Set up again with the microphone, it plays on.
    const uint64_t played = playing.frames;
    CHECK(waitFor([&] { return playing.frames > played + 4800; }));
    capture->stop();
    const uint64_t stopped = frames;
    CHECK(waitFor([&] { return [AVAudioSession.sharedInstance.category isEqualToString:AVAudioSessionCategoryPlayback]; }));
    CHECK(frames == stopped); // Nothing after stop().
    player->stop();
}

// UIKit's notifications move the application's state as sessions see it.
void appState() {
    std::atomic<int> changes{0};
    std::atomic<bool> background{false};
    const auto watch = seams::watchAppState([&](bool now) {
        background = now;
        ++changes;
    });
    std::this_thread::sleep_for(200ms);  // No UIApplication here: in front, as read on the main queue.
    CHECK(!seams::appInBackground());
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidEnterBackgroundNotification object:nil];
    CHECK(waitFor([&] { return background.load(); }) && seams::appInBackground());
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
    CHECK(waitFor([&] { return !background.load(); }) && !seams::appInBackground());
    const int seen = changes;
    [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
    std::this_thread::sleep_for(100ms);
    CHECK(changes == seen); // Active again from active: no change.
}

// A pasteboard through the clipboard module's system clipboard: what it
// places reads back, images in PNG, a concealed copy marked. One of the
// test's own: iOS asks the user before an application that is not in front
// reads the general one ("Allow Paste"), which no test can answer.
void clipboardCopies() {
    UIPasteboard* own = [UIPasteboard pasteboardWithUniqueName];
    auto board = clipboard::createPasteboardClipboard((__bridge void*)own);
    CHECK(clipboard::platformFormats() == clipboard::allFormats);
    const uint64_t before = board->changeCount();
    const std::string text = "iPad 剪贴板 🙂", html = "<b>bold</b>";
    clipboard::Content copy;
    copy.items.push_back({RCS_CLIPBOARD_TEXT, clipboard::Bytes(text.begin(), text.end())});
    copy.items.push_back({RCS_CLIPBOARD_HTML, clipboard::Bytes(html.begin(), html.end())});
    const uint64_t after = board->write(copy);
    CHECK(after != before && board->changeCount() == after);
    // (iOS adds an RTF of the HTML by itself.)
    auto read = board->read(clipboard::allFormats, 1 << 20);
    const auto find = [&](rcs_clipboard_format format) {
        for (const auto& item : read.items) {
            if (item.format == format) {
                return std::string(item.bytes.begin(), item.bytes.end());
            }
        }
        return std::string("(none)");
    };
    CHECK(find(RCS_CLIPBOARD_TEXT) == text && find(RCS_CLIPBOARD_HTML) == html && !read.concealed);
    // Only the formats asked for, and what does not fit is left out.
    read = board->read(1u << RCS_CLIPBOARD_HTML, 4);
    CHECK(read.items.empty() && (read.leftOut & (1u << RCS_CLIPBOARD_TEXT)) && (read.leftOut & (1u << RCS_CLIPBOARD_HTML)));
    // An image (a 2x2 PNG drawn here), and a concealed copy.
    UIGraphicsImageRenderer* renderer = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(2, 2)];
    NSData* png = [renderer PNGDataWithActions:^(UIGraphicsImageRendererContext* context) {
      [UIColor.redColor setFill];
      [context fillRect:CGRectMake(0, 0, 2, 2)];
    }];
    clipboard::Content image;
    image.concealed = true;
    const auto* bytes = static_cast<const uint8_t*>(png.bytes);
    image.items.push_back({RCS_CLIPBOARD_IMAGE, clipboard::Bytes(bytes, bytes + png.length)});
    board->write(image);
    read = board->read(clipboard::allFormats, 1 << 20);
    CHECK(read.concealed && read.items.size() == 1 && read.items[0].format == RCS_CLIPBOARD_IMAGE &&
          read.items[0].bytes.size() == png.length);
}

// Files on a pasteboard of the test's own: placed whole, they read back as
// files copied into the files directory; the other end's are providers that
// fetch when loaded (here a session's half of the test's writes them where
// asked), once for all of them.
void clipboardFiles() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "rcsdk-test-ios-files";
    fs::remove_all(root);
    fs::create_directories(root / "out");
    fs::create_directories(root / "own");
    { std::ofstream(root / "out" / "a.txt") << "alpha"; }
    UIPasteboard* own = [UIPasteboard pasteboardWithUniqueName];
    auto board = clipboard::createPasteboardClipboard((__bridge void*)own);
    std::mutex mutex;
    std::vector<clipboard::WantMessage> wanted;
    std::weak_ptr<clipboard::Pastes> broker;
    auto pastes = std::make_shared<clipboard::Pastes>([&](const clipboard::ToSession& message) {
        if (const auto* want = std::get_if<clipboard::WantMessage>(&message)) {
            { std::ofstream(fs::path(want->directory) / "report.pdf") << "pdf bytes"; }
            {
                std::lock_guard lock(mutex);
                wanted.push_back(*want);
            }
            broker.lock()->ended(clipboard::EndedMessage{want->request, RCS_OK, {}, true});
        }
    });
    broker = pastes;
    pastes->configure((root / "own").string(), true);
    board->usePastes(pastes);
    std::string why;
    CHECK(clipboard::platformFiles(why) == (RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE));

    clipboard::Content whole;
    whole.files = clipboard::Files{false, {}, {{"a.txt", RCS_FILE_REGULAR, 5, 0, (root / "out" / "a.txt").string()}}, 0};
    board->write(whole);
    const auto read = board->read(clipboard::knownFormats, 1 << 20);
    CHECK(read.files && read.files->entries.size() == 1 && read.files->entries[0].name == "a.txt" &&
          read.files->entries[0].size == 5);
    {
        std::ifstream in(read.files->entries[0].path);
        CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == "alpha");
    }

    clipboard::Content theirs;
    theirs.files = clipboard::Files{false, {}, {{"report.pdf", RCS_FILE_REGULAR, 9, 0, ""}}, 7};
    theirs.files->publication.fill(0x5c);
    board->write(theirs);
    NSItemProvider* provider = own.itemProviders.firstObject;
    CHECK(provider && [provider.suggestedName isEqualToString:@"report.pdf"]);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block std::string loaded;
    [provider loadFileRepresentationForTypeIdentifier:provider.registeredTypeIdentifiers.firstObject
                                    completionHandler:^(NSURL* url, NSError*) {
                                      if (url) {
                                          std::ifstream in(url.fileSystemRepresentation);
                                          loaded.assign(std::istreambuf_iterator<char>(in), {});
                                      }
                                      dispatch_semaphore_signal(done);
                                    }];
    CHECK(!dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t(10) * NSEC_PER_SEC)));
    CHECK(loaded == "pdf bytes");
    {
        std::lock_guard lock(mutex);
        CHECK(wanted.size() == 1 && wanted[0].placing == 7 && wanted[0].mode == RCS_FILE_FETCH_INTO);
    }
    fs::remove_all(root);
}

// The power module's requests: the device kept from locking (DISPLAY); the
// system alone is refused, with what to ask for instead.
void powerRequests() {
    CHECK(power::systemAvailable());
    auto display = power::systemRequest(RCS_KEEP_AWAKE_DISPLAY, "test");
    CHECK(display != nullptr);
    try {
        power::systemRequest(RCS_KEEP_AWAKE_SYSTEM, "test");
        CHECK(false);
    } catch (const power::Refused& refused) {
        CHECK(refused.result() == RCS_ERROR_UNSUPPORTED &&
              std::string(refused.what()).find("RCS_KEEP_AWAKE_DISPLAY") != std::string::npos);
    }
    display.reset();
}

} // namespace

int main() {
    return runTest("ios_platform", [] {
        // The main queue runs beside the test (the state is read there).
        std::thread test([] {
            audioSession();
            microphone();
            appState();
            clipboardCopies();
            clipboardFiles();
            powerRequests();
            dispatch_async(dispatch_get_main_queue(), ^{
              CFRunLoopStop(CFRunLoopGetMain());
            });
        });
        CFRunLoopRun();
        test.join();
    });
}
