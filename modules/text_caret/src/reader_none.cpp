// Systems where the module cannot tell where text goes.
#include "reader.hpp"

namespace rcsdk::text_caret {

std::unique_ptr<Reader> createPlatformReader(rcs_module) {
    return nullptr;
}

bool platformAvailable() noexcept {
    return false;
}

} // namespace rcsdk::text_caret
