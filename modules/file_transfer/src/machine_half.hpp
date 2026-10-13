// The module's machine half (target SYSTEM): where a sharer's agent runs, or
// in a viewer's own process, it reads and writes the files the session's
// half asks for (store.hpp) and answers it.
#pragma once

#include "store.hpp"

namespace rcsdk::file_transfer {

class Machine {
public:
    explicit Machine(rcs_module module);
    // Its calls, one at a time.
    void fromSession(std::span<const uint8_t> data);

private:
    const rcs_module module_;
    Worker worker_; // Last: it answers through module_.
};

} // namespace rcsdk::file_transfer
