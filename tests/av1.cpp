// AV1 OBU splitting, sequence header fields and the av1C record.
#include "media/av1.hpp"
#include "support/check.hpp"

using namespace rcsdk;
using Bytes = std::vector<uint8_t>;

int main() {
    return runTest("av1", [] {
        // A real sequence header from NVENC (640x360, Main, level 4.0, 8-bit 4:2:0).
        const Bytes header{0x0a, 0x0f, 0x00, 0x00, 0x00, 0x42, 0x62, 0x7f, 0xb3,
                           0xf0, 0x00, 0x86, 0x64, 0x08, 0x08, 0x04, 0x10};
        const auto info = av1::sequenceInfo(header);
        CHECK(info.profile == 0 && info.level == 8 && info.tier == 0);
        CHECK(!info.highBitDepth && !info.twelveBit && !info.monochrome);
        CHECK(info.subsamplingX == 1 && info.subsamplingY == 1 && info.chromaSamplePosition == 0);
        Bytes record{0x81, 0x08, 0x0c, 0x00};
        record.insert(record.end(), header.begin(), header.end());
        CHECK(av1::codecConfiguration(header) == record);

        // Temporal delimiter, sequence header, a frame OBU with a multi-byte size.
        Bytes unit{0x12, 0x00};
        unit.insert(unit.end(), header.begin(), header.end());
        const Bytes frame = [] {
            Bytes obu{0x32, 0x80, 0x01}; // OBU_FRAME, size 128 as two leb128 bytes.
            obu.resize(3 + 128, 0xab);
            return obu;
        }();
        unit.insert(unit.end(), frame.begin(), frame.end());
        const auto split = av1::obus(unit);
        CHECK(split.size() == 3);
        CHECK(split[0].type == av1::obuTemporalDelimiter && split[1].type == av1::obuSequenceHeader);
        CHECK(split[2].type == 6 && Bytes(split[2].bytes.begin(), split[2].bytes.end()) == frame);

        // Malformed input is rejected.
        CHECK(throws<Failure>([&] { av1::obus(Bytes(unit.begin(), unit.end() - 1)); })); // Truncated.
        CHECK(throws<Failure>([] { av1::obus(Bytes{0x10, 0x00}); }));                    // No size field.
        CHECK(throws<Failure>([] { av1::obus(Bytes{0x92, 0x00}); }));                    // Forbidden bit.
        CHECK(throws<Failure>([] { av1::obus(Bytes{0x12, 0xff}); })); // Unterminated size.
        CHECK(throws<Failure>([&] { av1::sequenceInfo(frame); }));    // Not a sequence header.
        for (size_t i = 3; i < header.size(); ++i) {                             // Header truncated inside its payload.
            Bytes cut(header.begin(), header.begin() + i);
            cut[1] = uint8_t(i - 2);
            CHECK(throws<Failure>([&] { av1::sequenceInfo(cut); }));
        }
        // A sequence header cut before or inside its size field, or longer
        // than its size says, is refused without reading past the buffer.
        for (size_t i = 0; i < header.size(); ++i) {
            CHECK(throws<Failure>([&] { av1::sequenceInfo(Bytes(header.begin(), header.begin() + i)); }));
        }
        CHECK(throws<Failure>([] { av1::sequenceInfo(Bytes{0x0a, 0x80}); }));
        Bytes trailing = header;
        trailing.push_back(0);
        CHECK(throws<Failure>([&] { av1::sequenceInfo(trailing); }));
        CHECK(throws<Failure>([&] { av1::codecConfiguration(Bytes{0x0a}); }));
        CHECK(av1::obus(header)[0].payload.size() == header[1]);
    });
}
