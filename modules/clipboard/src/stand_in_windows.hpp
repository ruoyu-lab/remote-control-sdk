// Windows: the other end's files on the clipboard as virtual files, the way
// Explorer pastes what is not on a disk here: a data object (placed with
// OleSetClipboard on the clipboard's STA thread) gives FileGroupDescriptorW,
// the files and directories with their relative paths, sizes and times, and
// FileContents, a stream per file that Explorer reads as it writes it, with
// its own progress dialog, cancellation and questions about names taken.
//
// A paste starts when the descriptor is asked for: a READ fetch of the
// copy's files, its listing waited for (30 s at most, dispatching COM calls
// and window messages meanwhile), and each stream's reads are pieces of it,
// 8 MiB asked ahead of where it reads. A read waits 30 s at most for its
// piece, then fails (Explorer offers to try again, skip or cancel). The
// fetch is closed once no stream of it has been open for 5 s (a paste that
// asks again then fetches again), or as the data object goes. Entries whose
// relative path does not fit MAX_PATH are left out of the descriptor (the
// log names them). Placed as a copy, never as a move: Explorer deletes
// nothing at the other end. A paste of files withdrawn fails.
//
// Unverified on a real Explorer paste: the asynchronous extraction
// (IDataObjectAsyncCapability) is not offered, so Explorer may wait for the
// listing on its own window's thread; whether it asks for FileContents
// streams one at a time and in order.
#pragma once

#include "pastes.hpp"

#include <windows.h>

namespace rcsdk::clipboard {

// A data object for `files` (placing named) with `formats` rendered beside
// them (text, the marks against history), its reference the caller's.
IDataObject* createStandIn(const Files& files, std::vector<std::pair<UINT, Bytes>> formats,
                           std::shared_ptr<Pastes> pastes);

} // namespace rcsdk::clipboard
