// Glass-to-glass latency of any remote desktop viewer on this Mac, read from
// the screen: the controlled machine runs `rcsdk_latency_target ... clock=IP:PORT`,
// whose marker shows this Mac's clock (milliseconds modulo 256) as of when
// it was drawn. This probe serves that clock over UDP, captures the viewer's
// windows as composited on the display (ScreenCaptureKit), and subtracts the
// marker from each frame's display time. Nothing in the path is the SDK's,
// so other software (Moonlight, ...) is measured the same way.
//
//   rcsdk_glass_probe --pid PID [--port 47911] [--seconds 20] [--report latency.csv]
//
// The result covers the controlled machine's composition, capture, encoding,
// network, decoding and this Mac's composition; it excludes the panel's scan-out.
#include "visual_marker.hpp"

#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <arpa/inet.h>
#include <mach/mach_time.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int64_t uptimeNs() {
    return int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

int64_t machToNs(uint64_t ticks) {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return int64_t(ticks * timebase.numer / timebase.denom);
}

std::atomic<uint64_t> clockRequests{0};

// Answers each 8-byte request with the request and this Mac's clock.
void serveClock(int port, std::atomic<bool>& stop) {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(uint16_t(port));
    address.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("clock socket");
        std::exit(1);
    }
    timeval timeout{0, 200'000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    while (!stop) {
        int64_t message[2];
        sockaddr_in peer{};
        socklen_t length = sizeof(peer);
        if (recvfrom(s, message, sizeof(int64_t), 0, reinterpret_cast<sockaddr*>(&peer), &length) ==
            ssize_t(sizeof(int64_t))) {
            message[1] = uptimeNs();
            sendto(s, message, sizeof(message), 0, reinterpret_cast<sockaddr*>(&peer), length);
            ++clockRequests;
        }
    }
    close(s);
}

struct Sample {
    double atSeconds, latencyMs;
};

} // namespace

@interface RCSGlassOutput : NSObject <SCStreamOutput>
@property(nonatomic) int64_t startNs;
@end

@implementation RCSGlassOutput {
    std::mutex _mutex;
    std::vector<Sample> _samples;
    int _last;
    uint64_t _frames;
}
- (instancetype)init {
    if ((self = [super init])) {
        _last = -1;
    }
    return self;
}
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)buffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen) {
        return;
    }
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(buffer, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) {
        return;
    }
    NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
    if ([info[SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete) {
        return;
    }
    const int64_t displayNs = machToNs([info[SCStreamFrameInfoDisplayTime] unsignedLongLongValue]);
    CVPixelBufferRef image = CMSampleBufferGetImageBuffer(buffer);
    if (!image || CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return;
    }
    const auto* pixels = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
    const size_t stride = CVPixelBufferGetBytesPerRow(image);
    const uint32_t width = uint32_t(CVPixelBufferGetWidth(image));
    const uint32_t height = uint32_t(CVPixelBufferGetHeight(image));
    // The viewer's picture may sit anywhere on the display: try rows across it.
    int value = -1;
    std::vector<uint8_t> gray(width);
    for (uint32_t y = height / 8; y < height * 7 / 8 && value < 0; y += std::max(1u, height / 200)) {
        const uint8_t* row = pixels + size_t(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            gray[x] = uint8_t((row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) / 3);
        }
        value = marker::decode(gray.data(), width);
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    std::lock_guard lock(_mutex);
    ++_frames;
    if (value < 0 || value == _last) {
        return; // No marker, or the same picture again.
    }
    _last = value;
    // The marker is the clock's whole milliseconds when drawn: on average 0.5 ms early.
    const double displayMs = double(displayNs) / 1e6;
    double latency = std::fmod(displayMs - value - 0.5, 256.0);
    if (latency < 0) {
        latency += 256;
    }
    _samples.push_back({double(displayNs - self.startNs) / 1e9, latency});
}
- (std::vector<Sample>)samples {
    std::lock_guard lock(_mutex);
    return _samples;
}
- (uint64_t)frames {
    std::lock_guard lock(_mutex);
    return _frames;
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        pid_t pid = 0;
        int port = 47911, seconds = 20;
        std::string report;
        for (int i = 1; i + 1 < argc; i += 2) {
            const std::string option = argv[i];
            if (option == "--pid") {
                pid = pid_t(std::stoi(argv[i + 1]));
            } else if (option == "--port") {
                port = std::stoi(argv[i + 1]);
            } else if (option == "--seconds") {
                seconds = std::stoi(argv[i + 1]);
            } else if (option == "--report") {
                report = argv[i + 1];
            }
        }
        if (!pid) {
            std::fprintf(stderr, "usage: rcsdk_glass_probe --pid PID [--port 47911] [--seconds 20] [--report FILE]\n");
            return 2;
        }
        std::atomic<bool> stop{false};
        std::thread clock([&] { serveClock(port, stop); });

        __block SCShareableContent* content = nil;
        dispatch_semaphore_t got = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* c, NSError* error) {
          content = c;
          if (error) {
              std::fprintf(stderr, "screen content: %s\n", error.localizedDescription.UTF8String);
          }
          dispatch_semaphore_signal(got);
        }];
        dispatch_semaphore_wait(got, DISPATCH_TIME_FOREVER);
        SCRunningApplication* application = nil;
        SCWindow* largest = nil;
        for (SCWindow* window in content.windows) {
            if (window.owningApplication.processID == pid && window.onScreen &&
                (!largest || window.frame.size.width * window.frame.size.height >
                                 largest.frame.size.width * largest.frame.size.height)) {
                largest = window;
                application = window.owningApplication;
            }
        }
        if (!largest) {
            std::fprintf(stderr, "process %d has no window on the current screen\n", pid);
            stop = true;
            clock.join();
            return 1;
        }
        SCDisplay* display = content.displays.firstObject;
        const CGPoint center = CGPointMake(CGRectGetMidX(largest.frame), CGRectGetMidY(largest.frame));
        for (SCDisplay* d in content.displays) {
            if (CGRectContainsPoint(d.frame, center)) {
                display = d;
            }
        }
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                                     includingApplications:@[ application ]
                                                          exceptingWindows:@[]];
        SCStreamConfiguration* configuration = [SCStreamConfiguration new];
        configuration.width = size_t(display.width);
        configuration.height = size_t(display.height);
        configuration.minimumFrameInterval = CMTimeMake(1, 240);
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        configuration.showsCursor = NO;
        configuration.queueDepth = 4;

        RCSGlassOutput* output = [RCSGlassOutput new];
        output.startNs = uptimeNs();
        SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:nil];
        dispatch_queue_t queue = dispatch_queue_create("glass", DISPATCH_QUEUE_SERIAL);
        NSError* error = nil;
        [stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:queue error:&error];
        [stream startCaptureWithCompletionHandler:^(NSError* e) {
          if (e) {
              std::fprintf(stderr, "capture: %s\n", e.localizedDescription.UTF8String);
              std::_Exit(1);
          }
        }];
        std::printf("capturing %s on a %.0fx%.0f display\n", application.applicationName.UTF8String,
                    double(display.width), double(display.height));
        size_t reported = 0;
        for (int s = 0; s < seconds; ++s) {
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
            const auto samples = [output samples];
            std::vector<double> second;
            for (size_t i = reported; i < samples.size(); ++i) {
                second.push_back(samples[i].latencyMs);
            }
            reported = samples.size();
            std::sort(second.begin(), second.end());
            std::printf("%2d s: %3zu new pictures, latency median %.1f ms, clock requests %llu\n", s + 1,
                        second.size(), second.empty() ? 0.0 : second[second.size() / 2],
                        static_cast<unsigned long long>(clockRequests.load()));
            std::fflush(stdout);
        }
        dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
        [stream stopCaptureWithCompletionHandler:^(NSError*) {
          dispatch_semaphore_signal(stopped);
        }];
        dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
        stop = true;
        clock.join();

        auto samples = [output samples];
        // The first two seconds cover the stream's start.
        samples.erase(std::remove_if(samples.begin(), samples.end(), [](const Sample& s) { return s.atSeconds < 2; }),
                      samples.end());
        if (!report.empty()) {
            std::ofstream file(report);
            file << "seconds,latency_ms\n";
            for (const Sample& s : samples) {
                file << s.atSeconds << ',' << s.latencyMs << '\n';
            }
        }
        if (samples.empty()) {
            std::printf("no marker seen (%llu frames captured)\n", static_cast<unsigned long long>([output frames]));
            return 1;
        }
        std::vector<double> latency;
        for (const Sample& s : samples) {
            latency.push_back(s.latencyMs);
        }
        std::sort(latency.begin(), latency.end());
        const auto at = [&](double p) {
            return latency[std::min(latency.size() - 1, size_t(p * double(latency.size())))];
        };
        double sum = 0;
        for (double l : latency) {
            sum += l;
        }
        const double span = samples.back().atSeconds - samples.front().atSeconds;
        std::printf("pictures %zu (%.1f per second), latency ms: min %.1f p5 %.1f median %.1f mean %.1f p95 %.1f max "
                    "%.1f\n",
                    latency.size(), span > 0 ? double(latency.size()) / span : 0.0, latency.front(), at(.05), at(.5),
                    sum / double(latency.size()), at(.95), latency.back());
    }
    return 0;
}
