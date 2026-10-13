// Adaptive bitrate decisions against a simulated network.
#include "protocol/rate_control.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <cstdio>

using namespace rcsdk;
using namespace rcsdk::protocol;

namespace {

// A bottleneck of `capacity` bits per second: what is sent beyond it queues
// (adding delay); a queue beyond 200 ms overflows (loss). Reports every 50 ms.
// Wireless: a receiver whose radio sleeps `sleepSteps` reports in every
// `sleepEvery` (its access point holds what comes meanwhile: no report, then
// the oldest of it late by the nap), and a sharer that sends only `sending`
// of the target (a still screen).
struct Simulation {
    double capacity;
    double queueBits = 0;
    int64_t propagationUs = 20'000;
    double lossRate = 0; // Random loss independent of load.
    int sleepEvery = 0, sleepSteps = 0;
    double sending = 1;
    uint32_t state = 12345;
    int step = 0;
    double heldBits = 0;

    struct Result {
        double meanTarget = 0, meanQueueMs = 0, maxQueueMs = 0, delivered = 0;
    };

    Result run(RateController& controller, double seconds, Clock::time_point& now) {
        Result result;
        const int steps = int(seconds / 0.05);
        for (int i = 0; i < steps; ++i) {
            const double sent = controller.target() * 0.05 * sending;
            queueBits += sent;
            const double drained = std::min(queueBits, capacity * 0.05);
            queueBits -= drained;
            double dropped = 0;
            const double limit = capacity * 0.2;
            if (queueBits > limit) {
                dropped = queueBits - limit;
                queueBits = limit;
            }
            const uint32_t expected = uint32_t(sent / 9600) + 1; // 1200-byte datagrams.
            uint32_t received = uint32_t(expected * (1 - dropped / std::max(sent, 1.0)));
            if (lossRate > 0) {
                uint32_t lost = 0;
                for (uint32_t p = 0; p < received; ++p) {
                    state = state * 1103515245 + 12345;
                    lost += (state >> 16) % 1000 < lossRate * 1000;
                }
                received -= lost;
            }
            RateReport report;
            report.windowUs = 50'000;
            report.expected = expected;
            report.received = received;
            report.bytes = uint64_t(drained / 8);
            report.delayUs = propagationUs + int64_t(queueBits / capacity * 1e6);
            report.sentBps = uint64_t(sent * 20);
            report.roundTripUs = uint64_t(2 * propagationUs);
            now += 50ms;
            const bool asleep = sleepEvery && step++ % sleepEvery < sleepSteps;
            if (asleep) {
                heldBits += drained;
            } else {
                if (heldBits > 0) { // Awake: the nap's arrivals at once, the oldest late by all of it.
                    report.bytes += uint64_t(heldBits / 8);
                    report.delayUs += int64_t(sleepSteps) * 50'000;
                    heldBits = 0;
                }
                controller.settle(controller.propose(report, now));
            }
            const double queueMs = queueBits / capacity * 1e3;
            result.meanTarget += controller.target() / steps;
            result.meanQueueMs += queueMs / steps;
            result.maxQueueMs = std::max(result.maxQueueMs, queueMs);
            result.delivered += drained;
        }
        result.delivered /= seconds;
        return result;
    }
};

} // namespace

int main() {
    return runTest("rate_control", [] {
        auto now = Clock::now();
        {
            // An open path: up to the host's maximum, and no further.
            RateController controller(1'000'000, 20'000'000, 5'000'000);
            Simulation network{1e9};
            network.run(controller, 30, now);
            CHECK(controller.target() == 20'000'000);
        }
        {
            // A 6 Mbit/s bottleneck under a 20 Mbit/s maximum: the target settles
            // near capacity, most of it is used, and queueing stays bounded.
            RateController controller(500'000, 20'000'000, 20'000'000);
            Simulation network{6e6};
            network.run(controller, 10, now); // Converge.
            const auto steady = network.run(controller, 60, now);
            std::printf("6 Mbit/s path: mean target %.2f Mbit/s, delivered %.2f, queue mean %.0f ms max %.0f ms\n",
                        steady.meanTarget / 1e6, steady.delivered / 1e6, steady.meanQueueMs, steady.maxQueueMs);
            CHECK(steady.meanTarget > 4.5e6 && steady.meanTarget < 7.5e6);
            CHECK(steady.delivered > 0.8 * 6e6);
            CHECK(steady.meanQueueMs < 80);
            // The path widens: the target follows.
            network.capacity = 15e6;
            network.run(controller, 30, now);
            CHECK(controller.target() > 11'000'000);
        }
        {
            // Slow start on a LAN (2 ms round trip): from 4 to 20 Mbit/s within
            // a second (probing 8 times as fast as on a long path), where a
            // 100 ms path still takes 3 s and more.
            RateController lan(500'000, 20'000'000, 4'000'000), far(500'000, 20'000'000, 4'000'000);
            Simulation fast{1e9}, slow{1e9};
            fast.propagationUs = 1'000;
            slow.propagationUs = 50'000;
            fast.run(lan, 1, now);
            slow.run(far, 3, now);
            std::printf("slow start: LAN %.1f Mbit/s after 1 s, 100 ms path %.1f Mbit/s after 3 s\n", lan.target() / 1e6,
                        far.target() / 1e6);
            CHECK(lan.target() == 20'000'000 && far.target() < 20'000'000);
            // A weak Wi-Fi LAN that carries 12 Mbit/s: the fast start overshoots
            // once and settles; its queue stays short of overflowing.
            RateController weak(500'000, 20'000'000, 4'000'000);
            Simulation wifi{12e6};
            wifi.propagationUs = 2'000;
            const auto first = wifi.run(weak, 3, now);
            const auto steady = wifi.run(weak, 20, now);
            std::printf("12 Mbit/s LAN: first 3 s queue max %.0f ms; then target %.1f Mbit/s, queue mean %.0f ms\n",
                        first.maxQueueMs, steady.meanTarget / 1e6, steady.meanQueueMs);
            CHECK(first.maxQueueMs < 200 && steady.meanTarget > 9e6 && steady.meanTarget < 13e6 &&
                  steady.meanQueueMs < 40);
            // A 2 Mbit/s bottleneck behind a short path (a hotspot's own LAN
            // side, a throttled link): down to it as before.
            RateController throttled(200'000, 6'000'000, 4'000'000);
            Simulation narrow{2e6};
            narrow.propagationUs = 5'000;
            narrow.run(throttled, 10, now);
            const auto settled = narrow.run(throttled, 20, now);
            std::printf("2 Mbit/s behind 10 ms: target %.2f Mbit/s, queue mean %.0f ms max %.0f ms\n",
                        settled.meanTarget / 1e6, settled.meanQueueMs, settled.maxQueueMs);
            CHECK(settled.meanTarget > 1.4e6 && settled.meanTarget < 2.6e6 && settled.meanQueueMs < 80);
        }
        {
            // 1% random loss alone is no reason to back off; 20% is.
            RateController light(1'000'000, 10'000'000, 10'000'000), heavy(1'000'000, 10'000'000, 10'000'000);
            Simulation a{1e9}, b{1e9};
            a.lossRate = 0.01;
            b.lossRate = 0.2;
            a.run(light, 10, now);
            b.run(heavy, 10, now);
            CHECK(light.target() == 10'000'000 && heavy.target() < 5'000'000);
        }
        {
            // Wi-Fi saving power at the viewer: every 2 s its radio naps 250 ms
            // and what came meanwhile arrives at once, 250 ms late. No queue of
            // the path's: the target climbs to the maximum all the same.
            RateController controller(500'000, 20'000'000, 10'000'000);
            Simulation network{1e9};
            network.sleepEvery = 40;
            network.sleepSteps = 5;
            network.run(controller, 30, now);
            std::printf("radio naps on an open path: target %.1f Mbit/s\n", controller.target() / 1e6);
            CHECK(controller.target() == 20'000'000);
        }
        {
            // A still screen after a squeeze (the sharer sends 5% of the
            // target) on a path that naps: neither the naps nor the little
            // arriving bring the target down.
            RateController controller(500'000, 20'000'000, 20'000'000);
            Simulation network{8e6};
            network.run(controller, 10, now); // An overuse: the target near 8 Mbit/s.
            const uint32_t before = controller.target();
            network.capacity = 1e9;
            network.sending = 0.05;
            network.sleepEvery = 40;
            network.sleepSteps = 5;
            network.run(controller, 20, now);
            std::printf("still screen after a squeeze, naps: %.1f -> %.1f Mbit/s\n", before / 1e6,
                        controller.target() / 1e6);
            CHECK(controller.target() >= before * 95 / 100);
        }
        {
            // A dip of 3 s to 4 Mbit/s on a 20 Mbit/s path: back near where it
            // was within 8 s (25% a second), not 8% a second.
            RateController controller(500'000, 20'000'000, 20'000'000);
            Simulation network{25e6};
            network.run(controller, 5, now);
            network.capacity = 4e6;
            network.run(controller, 3, now);
            const uint32_t low = controller.target();
            network.capacity = 25e6;
            network.run(controller, 8, now);
            std::printf("3 s dip: %.1f Mbit/s, 8 s later %.1f Mbit/s\n", low / 1e6, controller.target() / 1e6);
            CHECK(low < 8'000'000 && controller.target() > 16'000'000);
        }
        {
            // 3% loss of its own (Wi-Fi), no queue: parity and resending repair
            // it; the bitrate still climbs to the maximum.
            RateController controller(500'000, 20'000'000, 5'000'000);
            Simulation network{1e9};
            network.lossRate = 0.03;
            network.run(controller, 30, now);
            CHECK(controller.target() == 20'000'000);
        }
        {
            // Limits apply at once and bound the target.
            RateController controller(1'000'000, 20'000'000, 20'000'000);
            controller.setLimits(1'000'000, 8'000'000);
            CHECK(controller.target() == 8'000'000);
            controller.setLimits(12'000'000, 30'000'000);
            CHECK(controller.target() == 12'000'000);
        }
        {
            // Queueing that grows to 50 ms and stays there: the default rules
            // back off; rules for a path whose delay swings on its own (100 ms
            // overuse, 60 ms still clear) hold the bitrate.
            const auto queueing = [&](RateController& controller) {
                RateReport report;
                report.windowUs = 50'000;
                report.expected = report.received = 50;
                report.bytes = 62'500; // 10 Mbit/s arrive.
                for (int i = 0; i < 60; ++i) {
                    report.delayUs = 20'000 + std::clamp(i - 10, 0, 10) * 5'000;
                    now += 50ms;
                    controller.settle(controller.propose(report, now));
                }
            };
            RateRules lax;
            lax.overuseQueueUs = 100'000;
            lax.severeQueueUs = 200'000;
            lax.clearQueueUs = 60'000;
            RateController strict(1'000'000, 10'000'000, 10'000'000), relaxed(1'000'000, 10'000'000, 10'000'000, lax);
            queueing(strict);
            queueing(relaxed);
            CHECK(strict.target() < 9'000'000 && relaxed.target() == 10'000'000);
            // Rules change while running.
            relaxed.setRules({});
            queueing(relaxed);
            CHECK(relaxed.target() < 9'000'000);
        }
        {
            // No increase at all: an open path keeps the start bitrate.
            RateRules still;
            still.probeIncrease = still.increase = still.recoveryIncrease = 0;
            RateController controller(1'000'000, 20'000'000, 5'000'000, still);
            Simulation network{1e9};
            network.run(controller, 10, now);
            CHECK(controller.target() == 5'000'000);
        }
        {
            // The host decides: its decision becomes the target (within the
            // limits), noticeable changes are reported, and the next proposal
            // builds on it.
            RateController controller(500'000, 20'000'000, 10'000'000);
            CHECK(controller.settle(1'000'000) == 1'000'000u && controller.target() == 1'000'000);
            CHECK(!controller.settle(1'020'000) && controller.target() == 1'020'000);
            CHECK(controller.settle(50'000'000) == 20'000'000u);
            CHECK(controller.settle(100'000) == 500'000u);
            controller.settle(2'000'000);
            RateReport clear;
            clear.windowUs = 50'000;
            clear.expected = clear.received = 50;
            clear.bytes = 12'500;
            now += 50ms;
            const uint32_t proposal = controller.propose(clear, now);
            CHECK(proposal > 2'000'000 && proposal < 2'100'000 && controller.target() == proposal);
        }
        {
            // The video moves to a path 30 ms longer (another network): without
            // starting over, the old path's low delay would stay the base for
            // 10 s and the new one look congested. newPath starts at half the
            // target (the rules' pathChange) and probes up from the new path's
            // own reports; 0 starts at the start bitrate. Counted.
            RateController controller(500'000, 20'000'000, 4'000'000);
            Simulation wired{1e9};
            wired.propagationUs = 2'000;
            wired.run(controller, 5, now);
            CHECK(controller.target() == 20'000'000 && controller.pathChanges() == 0);
            controller.newPath();
            CHECK(controller.target() == 10'000'000 && controller.pathChanges() == 1 &&
                  controller.queueDelayUs() == 0 && controller.lossPermille() == 0);
            Simulation longer{1e9};
            longer.propagationUs = 32'000;
            const auto after = longer.run(controller, 10, now);
            std::printf("new path 30 ms longer: target %.1f Mbit/s after 10 s\n", controller.target() / 1e6);
            CHECK(controller.target() == 20'000'000 && after.maxQueueMs < 1);
            RateRules fresh;
            fresh.pathChange = 0;
            fresh.pathStartBps = 3'000'000;
            controller.setRules(fresh);
            controller.newPath();
            CHECK(controller.target() == 3'000'000 && controller.pathChanges() == 2);
        }
        {
            // Forged reports (the peer is authenticated but may be anyone's):
            // their extremes stay in range (UBSan in the ASan build).
            RateController controller(500'000, 20'000'000, 10'000'000);
            RateReport forged;
            forged.windowUs = 1;
            forged.expected = forged.received = 1;
            forged.bytes = UINT64_MAX;
            forged.delayUs = INT64_MIN;
            now += 50ms;
            controller.propose(forged, now);
            forged.delayUs = INT64_MAX;
            for (int i = 0; i < 3; ++i) { // Queueing counts once it persists (overusePersist).
                now += 50ms;
                controller.propose(forged, now);
            }
            CHECK(controller.receivedBps() <= uint64_t(1e12) && controller.queueDelayUs() > 0 &&
                  controller.target() >= 500'000 && controller.target() <= 20'000'000);
        }
    });
}
