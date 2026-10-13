// macOS: the other end's files on the pasteboard, fetched where Finder pastes
// them. Finder pastes only files that exist, and a promise of files only
// where they are dragged, so the pasteboard holds one file URL that a data
// provider gives as it is asked for: to Finder (the frontmost application)
// it is a hidden placeholder of the copy's, ".rcsdk-paste-<publication>", 0
// bytes, in the files directory. Finder copies it where the user pastes; an
// FSEvents stream over the home directory and /Volumes knows it by its name
// alone (nothing else is looked at, no file opened), and the copy's files are
// fetched into that directory (names taken are renamed, nothing replaced),
// where the placeholder copied is deleted then. Another program that pastes
// gets no file (an empty one would be a file that lies); the paste is
// reported. A copy moved there (Command-Option-V) is a rename: the
// placeholder is made again where it was.
//
// Unverified on a Mac with Finder: on which thread the provider is asked
// (the main run loop, most likely: a process that does not run it is never
// asked), whether it is asked again after it gave nothing, whether FSEvents
// tells of protected folders (Desktop, Documents) without asking the user.
#pragma once

#include "pastes.hpp"

#include <functional>

#ifdef __OBJC__
@class NSPasteboard;
#else
typedef struct objc_object NSPasteboard;
#endif

namespace rcsdk::clipboard {

class StandIn {
public:
    StandIn(const Files& files, std::shared_ptr<Pastes> pastes);
    // The watch ends, the placeholder of the copy goes (copies of it under way
    // are not waited for).
    ~StandIn();
    StandIn(const StandIn&) = delete;
    StandIn& operator=(const StandIn&) = delete;
    // Puts its file URL item on `board` (cleared before).
    bool place(NSPasteboard* board);

    struct State;

private:
    std::shared_ptr<State> state_;
};

// For tests: who pastes (a bundle id, as the frontmost application's) and
// which directories are watched for the placeholder copied; empty
// restores the system's.
void setStandInSeams(std::function<std::string()> pastingApplication, std::vector<std::string> roots);
// Whether this process runs its main run loop, where the provider is asked.
bool mainRunLoopRuns();

} // namespace rcsdk::clipboard
