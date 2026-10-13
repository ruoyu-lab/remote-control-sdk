// The browser's decoder (src/platform/web/decoded.hpp), apart from the
// browser: a picture of a size other than the stream's is not shown (a
// canvas would take its size), and only the picture of the chunk waited for
// is kept: a second picture of one chunk and a late one are not (their
// VideoFrames are closed at once instead of piling up).
#include "platform/web/decoded.hpp"
#include "support/check.hpp"

using namespace rcsdk::platform;

int main() {
    return runTest("web_decoded", [] {
        CHECK(sizeProblem(1920, 1080, 1920, 1080).empty());
        const auto larger = sizeProblem(16384, 16384, 1920, 1080);
        CHECK(larger.find("16384x16384") != std::string::npos && larger.find("1920x1080") != std::string::npos);
        CHECK(!sizeProblem(1920, 1088, 1920, 1080).empty() && !sizeProblem(960, 540, 1920, 1080).empty());

        DecodedSlot slot;
        CHECK(!slot.keep({1, 1920, 1080, 1, {}})); // No decode() waits.
        slot.waiting = 2;
        CHECK(!slot.keep({2, 1920, 1080, 1, {}})); // Of a chunk given up on.
        CHECK(slot.keep({3, 1920, 1080, 2, {}}));
        CHECK(!slot.keep({4, 1920, 1080, 2, {}}));       // A second picture of the chunk.
        CHECK(slot.keep({0, 0, 0, 0, "decoder error"})); // Errors are kept.
        CHECK(slot.outcomes.size() == 2 && slot.outcomes.front().frame == 3);
        // A flood of pictures for one chunk keeps one.
        slot.outcomes.clear();
        slot.waiting = 3;
        size_t kept = 0;
        for (int frame = 10; frame < 1010; ++frame) {
            kept += slot.keep({frame, 1920, 1080, 3, {}});
        }
        CHECK(kept == 1 && slot.outcomes.size() == 1);
    });
}
