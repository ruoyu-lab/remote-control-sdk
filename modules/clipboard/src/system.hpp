// The system clipboard. Each platform has one (system_<os>); a test puts a
// clipboard of its own in its place (setFactory).
#pragma once

#include "content.hpp"

#include <functional>
#include <memory>

namespace rcsdk::clipboard {

class Pastes;

// The system clipboard, for the thread that made it (Windows: its window lives
// there), which uses and destroys it.
class SystemClipboard {
public:
    virtual ~SystemClipboard() = default;
    // Moves on whenever the clipboard changes (Windows: its sequence number;
    // macOS: the pasteboard's change count).
    virtual uint64_t changeCount() = 0;
    // What it holds now in `formats`, in format order while they fit
    // `maxBytes`; the formats it holds besides are leftOut. FILES: the files
    // copied, by path (their names and sizes count). Throws Error(DEVICE)
    // when the system does not let it be read now (another program has it
    // open).
    virtual Content read(uint32_t formats, size_t maxBytes) = 0;
    // Replaces what it holds (with the marks of a concealed copy); returns
    // changeCount() after. Files with paths go as the files themselves;
    // without, as a stand-in whose pastes ask `pastes` for them (files.placing
    // names the copy). Throws Error(DEVICE).
    virtual uint64_t write(const Content& content) = 0;
    // Where its stand-ins ask for the other end's files, and its files
    // directory: set once, before anything is read or written.
    void usePastes(std::shared_ptr<Pastes> pastes) { pastes_ = std::move(pastes); }

protected:
    std::shared_ptr<Pastes> pastes_;
};

// The platform's. Throws Error(UNSUPPORTED) where it has none for this process.
std::unique_ptr<SystemClipboard> createPlatformClipboard();
// iOS: the same on `pasteboard` (a UIPasteboard*) instead of the general one;
// macOS: on an NSPasteboard*. Tests use one of their own, which the system
// never asks the user about and nobody else reads.
std::unique_ptr<SystemClipboard> createPasteboardClipboard(void* pasteboard);
// The formats the platform's carries (FILES aside).
uint32_t platformFormats() noexcept;
// Whether this system's clipboard does anything with files at all...
bool platformCarriesFiles() noexcept;
// ...and what this process's does (RCS_CLIPBOARD_SEND, RCS_CLIPBOARD_RECEIVE),
// with why not where one is missing.
uint32_t platformFiles(std::string& why);

// The clipboard of `role`'s ends (a sharer's agent, a viewer) is `factory`'s
// instead of the system's, and this system counts as carrying every format,
// files both ways too; empty restores.
using Factory = std::function<std::unique_ptr<SystemClipboard>(rcs_role role)>;
void setFactory(Factory factory);
std::unique_ptr<SystemClipboard> createClipboard(rcs_role role);
// rcs_clipboard_system_formats, and the two above as tests make them.
uint32_t systemFormats() noexcept;
bool systemCarriesFiles() noexcept;
uint32_t systemFiles(std::string& why);

} // namespace rcsdk::clipboard
