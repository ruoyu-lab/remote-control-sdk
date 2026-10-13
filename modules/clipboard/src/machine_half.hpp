// The module's machine half: this machine's clipboard (a sharer's agent's, a
// viewer's own), watched and written by a Watcher while the session half
// says, its news sent back to the session half; the system pastes of the
// other end's files ask through Pastes. It tells the session half first what
// this process's clipboard does with files.
#pragma once

#include "pastes.hpp"

#include <memory>

namespace rcsdk::clipboard {

class Machine {
public:
    Machine(rcs_module module, rcs_role role);
    // Its calls, one at a time.
    void fromSession(std::span<const uint8_t> data);
    void stop();

private:
    // A copy placed whose files are fetched ahead into the files directory:
    // once they are, it is placed again as those files.
    struct Ahead {
        uint64_t placing = 0;
        Content content;
        std::string directory;
    };
    void tell(const ToSession& message);
    void place(PlaceMessage place);

    const rcs_module module_;
    const rcs_role role_;
    const std::shared_ptr<Pastes> pastes_;
    bool told_ = false;                // What it does with files.
    std::map<uint64_t, Ahead> aheads_; // By fetch.
    std::unique_ptr<Watcher> watcher_; // While used.
};

} // namespace rcsdk::clipboard
