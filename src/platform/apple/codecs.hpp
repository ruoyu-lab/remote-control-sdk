// The VideoToolbox implementations each Apple system lists (codecs.mm).
#pragma once

#include "platform/platform.hpp"

#include <vector>

namespace rcsdk::platform::apple {

// The encoders and the decoders, best first within a codec, direction and
// acceleration; each with its codec's own size limit (VideoToolbox says
// nothing of the sizes it takes).
std::vector<CodecImplementation> encoderImplementations();
std::vector<CodecImplementation> decoderImplementations();

// Whether this device decodes `codec` in hardware. VideoToolbox decodes VP9
// only for processes that ask for it (macOS 11, iOS 26.2 and later), which
// this does the first time.
bool hardwareDecodes(rcs_codec codec);

} // namespace rcsdk::platform::apple
