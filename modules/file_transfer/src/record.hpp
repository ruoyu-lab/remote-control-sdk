// A transfer's record in the directory it is received into,
// ".rcsdk-resume-<token in hex>": the files of it an earlier run started
// (each with its size and time at the sending end) and those it completed
// (and as what), so that a later run of the same transfer goes on from
// there. Written only with keep_partial; deleted once the transfer
// completes, is cancelled or fails otherwise, or the host discards it.
//
// Its form: "RCSDKFT1", the token, then entries, each a type (1 started, 2
// completed), the size (u64), the time (i64), the path (u16 size, UTF-8)
// and, completed, where it went (the same). What is read is checked like
// anything from the other end: a record that is not one is no record, and
// entries after a malformed one are not taken.
#pragma once

#include "files.hpp"
#include "messages.hpp"

#include <map>
#include <optional>

namespace rcsdk::file_transfer::record {

struct Entry {
    uint64_t size = 0;
    int64_t modified = 0;
    bool completed = false;
    std::string saved; // Completed: where it went, relative.
};

// What the record in `directory` holds, by path (the newest entry of each); empty without one.
std::map<std::string, Entry> read(const files::Directory& directory, const Token& token);
// An entry added (the record made with its first).
void started(const files::Directory& directory, const Token& token, const ItemSpec& item);
void completed(const files::Directory& directory, const Token& token, const ItemSpec& item,
               const std::string& saved);
// The record goes.
void remove(const files::Directory& directory, const Token& token);

} // namespace rcsdk::file_transfer::record
