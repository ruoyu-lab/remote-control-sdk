#include "store.hpp"

#include "paths.hpp"
#include "record.hpp"

#include <set>

namespace rcsdk::file_transfer {
namespace {

constexpr uint32_t mostNumbers = 9999; // Names RENAME tries.
// What answers call the directory a transfer goes into: its host knows the path, the other end needs none.
const std::string receiving = "the receiving directory";

bool same(const record::Entry& entry, const ItemSpec& item) {
    return entry.size == item.size && entry.modified == item.modified;
}

// Where a transfer writes a file until it is whole: "<name>.<token>.rcsdk-part",
// all of the transfer's token in hex, so that two transfers of the same name
// into one directory never write each other's, whatever tokens their hosts
// choose (and a resumed transfer, which keeps its token, finds its own).
// Of a path, the path of its partial file; of a name, its name.
std::string partOf(const std::string& path, const Token& token) {
    return path + "." + hex(token) + std::string(partSuffix);
}

// Deletes what is at `path` under `root` (a file, or a link itself); nothing
// when it is not there.
void removeIn(const files::Directory& root, const std::string& path) {
    if (const auto in = root.find(parentOf(path))) {
        in->remove(lastComponent(path));
    }
}

} // namespace

std::optional<Reply> Store::handle(Request request) {
    if (const auto* r = std::get_if<ListRequest>(&request)) {
        return list(*r);
    }
    if (const auto* r = std::get_if<PrepareRequest>(&request)) {
        return prepare(*r);
    }
    if (const auto* r = std::get_if<ReadRequest>(&request)) {
        return read(*r);
    }
    if (const auto* r = std::get_if<WriteRequest>(&request)) {
        return write(*r);
    }
    if (const auto* r = std::get_if<FinishRequest>(&request)) {
        return finish(*r);
    }
    if (const auto* r = std::get_if<DiscardRequest>(&request)) {
        discard(*r);
        if (!r->ended) {
            return std::nullopt;
        }
        reserved_.erase(r->transfer);
        return Ended{r->transfer};
    }
    if (const auto* r = std::get_if<CompleteRequest>(&request)) {
        complete(*r);
        reserved_.erase(r->transfer);
        return Ended{r->transfer};
    }
    if (const auto* r = std::get_if<CloseRequest>(&request)) {
        close(r->transfer);
        reserved_.erase(r->transfer);
        return Ended{r->transfer};
    }
    return purge(std::get<PurgeRequest>(request));
}

void Store::shutdown() {
    readers_.clear();
    writers_.clear();
    for (const auto& [where, partial] : partials_) {
        if (!partial.keep) {
            try {
                removeIn(files::Directory::open(where.first, receiving), where.second);
            } catch (const Error& error) {
                log(error.what());
            }
        }
    }
    partials_.clear();
    reserved_.clear();
}

// ---- Sending ----------------------------------------------------------------------------

Listed Store::list(const ListRequest& request) {
    Listed out;
    out.job = request.job;
    try {
        size_t bytes = 64;
        std::set<std::string> names;
        const auto add = [&](ItemSpec item, std::string relative) {
            if (out.items.size() >= request.maxItems) {
                fail(RCS_ERROR_INVALID_ARGUMENT, "More than " + std::to_string(request.maxItems) +
                                                     " items (max_items, this end's or the other's)");
            }
            bytes += 29 + item.path.size() + relative.size();
            if (bytes > maxMessageBytes - 1024) {
                fail(RCS_ERROR_INVALID_ARGUMENT, "Too many items, or names too long, for one transfer");
            }
            if (!names.insert(item.path).second) {
                fail(RCS_ERROR_INVALID_ARGUMENT, "Two items go as \"" + item.path + "\"");
            }
            out.items.push_back(std::move(item));
            out.sources.emplace_back(uint32_t(out.bases.size() - 1), std::move(relative));
        };
        for (size_t e = 0; e < request.entries.size(); ++e) {
            const auto& [source, given] = request.entries[e];
            const std::string entry = "Entry " + std::to_string(e);
            if (!files::absolute(source)) {
                fail(RCS_ERROR_INVALID_ARGUMENT, entry + " is not a full path");
            }
            const std::string name = given.empty() ? files::lastName(source) : given;
            if (name.empty()) {
                fail(RCS_ERROR_INVALID_ARGUMENT, entry + " has no name to go as: give it one (rcs_file_item.name)");
            }
            if (const auto problem = pathProblem(name)) {
                fail(RCS_ERROR_INVALID_ARGUMENT, entry + " cannot go as \"" + name + "\": " + *problem);
            }
            const auto top = files::status(source, name);
            if (!top) {
                fail(RCS_ERROR_INVALID_ARGUMENT, name + " is not there");
            }
            const bool directory = top->kind == RCS_FILE_DIRECTORY;
            out.bases.push_back(directory ? source : files::parentPath(source));
            bytes += source.size() + 6;
            if (!directory) {
                add({top->kind, top->size, top->modified, name},
                    top->kind == RCS_FILE_REGULAR ? files::lastName(source) : std::string());
                continue;
            }
            add({RCS_FILE_DIRECTORY, 0, top->modified, name}, {});
            // Its files are read through its directory (a file's own), then
            // their names below it, never through a link put there later; so
            // is it listed. Depth first, each directory before what it holds,
            // in name order: the directory it is in and its name there, the
            // name it goes as, and the names below the base.
            struct Next {
                files::Directory in;
                std::string name, as, relative;
            };
            std::vector<Next> next;
            const auto holds = [&](const files::Directory& at, const std::string& as, const std::string& relative) {
                const auto children = at.list();
                for (auto child = children.rbegin(); child != children.rend(); ++child) {
                    const std::string childAs = as + "/" + *child;
                    if (const auto problem = pathProblem(childAs)) {
                        fail(RCS_ERROR_INVALID_ARGUMENT, "\"" + childAs + "\" cannot go: " + *problem);
                    }
                    next.push_back({at, *child, childAs, relative.empty() ? *child : relative + "/" + *child});
                }
            };
            holds(files::Directory::open(source, name, name), name, {});
            while (!next.empty()) {
                const Next at = std::move(next.back());
                next.pop_back();
                const auto status = at.in.status(at.name);
                if (!status) {
                    fail(RCS_ERROR_INVALID_ARGUMENT, at.as + " is not there");
                }
                if (status->kind != RCS_FILE_DIRECTORY) {
                    add({status->kind, status->size, status->modified, at.as},
                        status->kind == RCS_FILE_REGULAR ? at.relative : std::string());
                    continue;
                }
                if (checked) {
                    checked(at.as);
                }
                std::optional<files::Directory> opened;
                try {
                    opened = at.in.find(at.name);
                } catch (const Error& error) {
                    if (error.result() != RCS_ERROR_INVALID_STATE) {
                        throw;
                    }
                    // A link (or a file) took its place since it was looked at: listed as neither, not followed.
                    add({RCS_FILE_OTHER, 0, status->modified, at.as}, {});
                    continue;
                }
                if (!opened) {
                    fail(RCS_ERROR_INVALID_ARGUMENT, at.as + " is not there");
                }
                add({RCS_FILE_DIRECTORY, 0, status->modified, at.as}, {});
                holds(*opened, at.as, at.relative);
            }
        }
    } catch (const Error& error) {
        out = Listed{request.job, error.result(), error.what(), {}, {}, {}};
    }
    return out;
}

Reply Store::read(const ReadRequest& request) {
    const std::string& name = request.name;
    try {
        files::File* file = opened(readers_, request.transfer, request.item);
        if (!file) {
            // The directories on its way are called as items go: the names
            // the host gave its entries stand for the machine's.
            const std::string tail = "/" + request.relative;
            const bool within = name.size() > tail.size() && name.ends_with(tail);
            const std::string top = within ? name.substr(0, name.size() - tail.size()) : std::string();
            const auto in = files::Directory::open(request.base, within ? top : "the directory of " + name, top)
                                .find(parentOf(request.relative));
            if (!in) {
                fail(RCS_ERROR_INVALID_STATE, name + " is gone");
            }
            auto opening = in->read(lastComponent(request.relative), name);
            const auto now = opening.status();
            if (now.kind != RCS_FILE_REGULAR || now.size != request.size || now.modified != request.modified) {
                fail(RCS_ERROR_INVALID_STATE, name + " changed after it was offered (its size or time)");
            }
            file = &keepOpen(readers_, request.transfer, request.item, std::move(opening));
        }
        Data data{request.transfer, request.epoch, request.item, request.offset, Bytes(request.length)};
        file->readAt(request.offset, data.data);
        if (request.offset + request.length == request.size) {
            close(request.transfer, request.item); // Its last piece.
        }
        return data;
    } catch (const Error& error) {
        close(request.transfer, request.item);
        return ReadFailed{request.transfer, request.epoch, request.item, error.result(), error.what()};
    }
}

// ---- Receiving ----------------------------------------------------------------------------

Prepared Store::prepare(const PrepareRequest& request) {
    Prepared out;
    out.transfer = request.transfer;
    const auto& directory = request.directory;
    try {
        if (!files::absolute(directory)) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "The receiving directory is not a full path");
        }
        const auto root = files::Directory::open(directory, receiving);
        // Only a transfer that resumes (its host saw it said so, with the
        // token) goes on from what a transfer of its token kept; another one
        // of that token (its sender chose it) neither takes it nor deletes it.
        const auto keptThere = [](const std::string& what) {
            fail(RCS_ERROR_INVALID_STATE, what + " of a transfer of this token is there, and this one does not resume "
                                                 "it: nothing of it is taken or deleted (rcs_file_discard deletes it)");
        };
        if (!request.resuming && root.status(recordName(request.token))) {
            keptThere("The record");
        }
        const auto kept = request.resuming ? record::read(root, request.token) : std::map<std::string, record::Entry>();
        uint64_t needed = 0;
        for (uint32_t i = 0; i < uint32_t(request.items.size()); ++i) {
            const auto& item = request.items[i];
            if (item.kind == RCS_FILE_OTHER) {
                continue;
            }
            const std::string name = lastComponent(item.path);
            // The item's directory (one not there yet is made as the item is
            // written): a link or a file in the way fails the item.
            std::optional<files::Directory> in;
            try {
                in = root.find(parentOf(item.path));
            } catch (const Error& error) {
                out.starts.push_back({i, Start::failed, 0, error.result(), error.what()});
                continue;
            }
            const auto there = in ? in->status(name) : std::nullopt;
            if (item.kind == RCS_FILE_DIRECTORY) {
                if (there && there->kind != RCS_FILE_DIRECTORY) {
                    const std::string why = item.path + " is there and is not a directory";
                    if (request.conflict == RCS_FILE_CONFLICT_FAIL) {
                        fail(RCS_ERROR_INVALID_STATE, why + " (on_conflict FAIL)");
                    }
                    out.starts.push_back({i, Start::failed, 0, RCS_ERROR_INVALID_STATE, why});
                }
                continue;
            }
            const auto found = kept.find(item.path);
            const bool earlier = found != kept.end() && same(found->second, item);
            // Saved by an earlier run in the item's directory (RENAME changes
            // only the last component).
            if (earlier && found->second.completed && in && parentOf(found->second.saved) == parentOf(item.path)) {
                const auto saved = in->status(lastComponent(found->second.saved));
                if (saved && saved->kind == RCS_FILE_REGULAR && saved->size == item.size) {
                    out.starts.push_back({i, Start::done, 0, RCS_OK, found->second.saved});
                    continue;
                }
            }
            // A partial file of this transfer's (its token names it) goes on
            // where the record says the transfer started that item, as it is now.
            const std::string partName = partOf(name, request.token);
            const auto part = in ? in->status(partName) : std::nullopt;
            if (part && !request.resuming) {
                keptThere("The partial file " + partOf(item.path, request.token));
            }
            uint64_t offset = 0;
            if (earlier && !found->second.completed && part && part->kind == RCS_FILE_REGULAR &&
                part->size <= item.size) {
                offset = part->size;
            }
            if (there) {
                if (request.conflict == RCS_FILE_CONFLICT_SKIP) {
                    out.starts.push_back(
                        {i, Start::skip, 0, RCS_OK, item.path + " is there; left as it is (on_conflict SKIP)"});
                    continue;
                }
                if (request.conflict == RCS_FILE_CONFLICT_FAIL) {
                    fail(RCS_ERROR_INVALID_STATE, item.path + " is there (on_conflict FAIL)");
                }
                if (request.conflict == RCS_FILE_CONFLICT_OVERWRITE && there->kind == RCS_FILE_DIRECTORY) {
                    out.starts.push_back({i, Start::failed, 0, RCS_ERROR_INVALID_STATE,
                                          item.path + " is a directory: a file does not replace it"});
                    continue;
                }
            }
            if (offset) {
                out.starts.push_back({i, Start::resume, offset, RCS_OK, {}});
            } else if (part && part->kind != RCS_FILE_DIRECTORY) {
                in->remove(partName); // What an earlier run left that does not go on: the item starts over.
            }
            needed += item.size - offset;
        }
        // What it needs, beside what the transfers going into the same volume still need.
        const uint64_t volume = root.volume();
        uint64_t going = 0;
        for (const auto& [transfer, reserved] : reserved_) {
            going += transfer != request.transfer && reserved.volume == volume ? reserved.bytes : 0;
        }
        const uint64_t available = files::freeSpace(directory);
        if (needed > available || going > available - needed) {
            fail(RCS_ERROR_DEVICE,
                 "Not enough space in the receiving directory: " + std::to_string(needed) + " bytes needed, " +
                     std::to_string(available) + " free" +
                     (going ? ", " + std::to_string(going) + " of them for transfers going there" : std::string()));
        }
        reserved_[request.transfer] = {volume, needed};
    } catch (const Error& error) {
        out.result = error.result();
        out.message = error.what();
        out.starts.clear();
    }
    return out;
}

Reply Store::write(const WriteRequest& request) {
    const std::string part = partOf(request.spec.path, request.token);
    try {
        files::File* file = opened(writers_, request.transfer, request.item);
        if (!file || file->position() != request.offset) {
            close(request.transfer, request.item);
            const auto root = files::Directory::open(request.directory, receiving);
            const auto in = root.make(parentOf(request.spec.path));
            if (checked) {
                checked(request.spec.path);
            }
            // A new partial file is this transfer's alone: one there already
            // is someone else's, never written over.
            const std::string name = lastComponent(part);
            auto opening = request.fresh && !request.offset ? in.create(name) : in.write(name, request.offset);
            partials_[{request.directory, part}] = {request.transfer, request.keep};
            if (!request.offset && request.keep) {
                record::started(root, request.token, request.spec);
            }
            file = &keepOpen(writers_, request.transfer, request.item, std::move(opening));
        }
        file->append(request.data);
        if (const auto reserved = reserved_.find(request.transfer); reserved != reserved_.end()) {
            reserved->second.bytes -= std::min<uint64_t>(reserved->second.bytes, request.data.size());
        }
        return Written{request.transfer, request.epoch, request.item, file->position()};
    } catch (const Error& error) {
        close(request.transfer, request.item);
        return WriteFailed{request.transfer, request.epoch, request.item, error.result(), error.what()};
    }
}

Finished Store::finish(const FinishRequest& request) {
    Finished out;
    out.transfer = request.transfer;
    out.epoch = request.epoch;
    out.item = request.item;
    const auto& directory = request.directory;
    const auto& spec = request.spec;
    const std::string part = partOf(spec.path, request.token);
    const std::string name = lastComponent(spec.path);
    const std::string partName = lastComponent(part);
    const auto failedWith = [&](rcs_result result, const std::string& why, bool whole = false) {
        out.state = RCS_FILE_ITEM_FAILED;
        out.result = result;
        out.message = why;
        out.whole = whole;
    };
    try {
        const auto root = files::Directory::open(directory, receiving);
        if (spec.kind == RCS_FILE_DIRECTORY) {
            root.make(spec.path);
            out.saved = spec.path;
            return out;
        }
        close(request.transfer, request.item);
        // The item's directory: made for an empty file (nothing was written).
        const auto in = spec.size ? root.find(parentOf(spec.path)) : root.make(parentOf(spec.path));
        if (in && checked) {
            checked(spec.path);
        }
        auto status = in ? in->status(partName) : std::nullopt;
        if (!status && !spec.size) {
            in->create(partName);
            partials_[{directory, part}] = {request.transfer, request.keep};
            status = in->status(partName);
        }
        if (!status || status->kind != RCS_FILE_REGULAR) {
            // An agent that left right after it gave the file its name: it is there, whole.
            const auto there = in ? in->status(name) : std::nullopt;
            if (!status && there && there->kind == RCS_FILE_REGULAR && there->size == spec.size &&
                there->modified == spec.modified && spec.modified) {
                out.saved = spec.path;
                return out;
            }
            failedWith(RCS_ERROR_INVALID_STATE, part + " is gone");
            return out;
        }
        if (status->size != spec.size) {
            in->remove(partName);
            partials_.erase({directory, part});
            failedWith(RCS_ERROR_INVALID_STATE,
                       part + " holds " + std::to_string(status->size) + " bytes, not " + std::to_string(spec.size));
            return out;
        }
        if (spec.modified) {
            in->setModified(partName, spec.modified);
        }
        // Its name: free, or as on_conflict says.
        std::string saved;
        if (in->rename(partName, name, false)) {
            saved = spec.path;
        } else if (request.conflict == RCS_FILE_CONFLICT_OVERWRITE) {
            const auto there = in->status(name);
            if (there && there->kind == RCS_FILE_DIRECTORY) {
                failedWith(RCS_ERROR_INVALID_STATE, spec.path + " is a directory: a file does not replace it");
            } else {
                in->rename(partName, name, true);
                saved = spec.path;
            }
        } else if (request.conflict == RCS_FILE_CONFLICT_RENAME) {
            for (uint32_t n = 1; n <= mostNumbers && saved.empty(); ++n) {
                const std::string candidate = numbered(spec.path, n);
                if (pathProblem(candidate)) {
                    break;
                }
                if (in->rename(partName, lastComponent(candidate), false)) {
                    saved = candidate;
                }
            }
            if (saved.empty()) {
                failedWith(RCS_ERROR_INVALID_STATE, "No free name for " + spec.path);
            }
        } else if (request.conflict == RCS_FILE_CONFLICT_SKIP) {
            out.state = RCS_FILE_ITEM_SKIPPED;
            out.message = spec.path + " is there; left as it is (on_conflict SKIP)";
        } else {
            failedWith(RCS_ERROR_INVALID_STATE, spec.path + " is there (on_conflict FAIL)", true);
        }
        if (saved.empty()) {
            in->remove(partName);
        } else if (request.keep) {
            record::completed(root, request.token, spec, saved);
        }
        partials_.erase({directory, part});
        out.saved = saved;
    } catch (const Error& error) {
        failedWith(error.result(), error.what());
        out.saved.clear();
        try {
            removeIn(files::Directory::open(directory, receiving), part);
            partials_.erase({directory, part});
        } catch (const Error& again) {
            log(again.what());
        }
    }
    return out;
}

void Store::discard(const DiscardRequest& request) {
    close(request.transfer); // What goes on opens again.
    for (const auto& path : request.paths) {
        partials_.erase({request.directory, partOf(path, request.token)});
    }
    try {
        const auto root = files::Directory::open(request.directory, receiving);
        for (const auto& path : request.paths) {
            try {
                removeIn(root, partOf(path, request.token));
            } catch (const Error& error) {
                log(error.what());
            }
        }
        if (request.ended) {
            record::remove(root, request.token);
        }
    } catch (const Error& error) {
        log(error.what());
    }
}

void Store::complete(const CompleteRequest& request) {
    close(request.transfer);
    auto directories = request.directories;
    std::stable_sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
        return std::count(a.first.begin(), a.first.end(), '/') > std::count(b.first.begin(), b.first.end(), '/');
    });
    try {
        const auto root = files::Directory::open(request.directory, receiving);
        for (const auto& [path, modified] : directories) {
            if (!modified) {
                continue;
            }
            try {
                const auto in = root.find(parentOf(path));
                if (!in) {
                    fail(RCS_ERROR_INVALID_STATE, path + " is gone: its time is not set");
                }
                in->setModified(lastComponent(path), modified);
            } catch (const Error& error) {
                log(error.what());
            }
        }
        record::remove(root, request.token);
    } catch (const Error& error) {
        log(error.what());
    }
}

Purged Store::purge(const PurgeRequest& request) {
    Purged out;
    out.job = request.job;
    const auto& directory = request.directory;
    const std::string shown = "the directory";
    try {
        if (!files::absolute(directory)) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "The directory is not a full path");
        }
        const auto root =
            files::status(directory, shown) ? std::optional(files::Directory::open(directory, shown)) : std::nullopt;
        if (!root || !root->status(recordName(request.token))) {
            out.message = "Nothing of that transfer is kept in the directory";
            return out;
        }
        for (const auto& [path, entry] : record::read(*root, request.token)) {
            const std::string part = partOf(path, request.token);
            const auto in = entry.completed ? std::nullopt : root->find(parentOf(path));
            const auto status = in ? in->status(lastComponent(part)) : std::nullopt;
            if (status && status->kind == RCS_FILE_REGULAR) {
                in->remove(lastComponent(part));
                partials_.erase({directory, part});
                ++out.files;
            }
        }
        record::remove(*root, request.token);
    } catch (const Error& error) {
        out.result = error.result();
        out.message = error.what();
    }
    return out;
}

// ---- Open files ---------------------------------------------------------------------------

files::File* Store::opened(std::deque<Open>& open, uint64_t transfer, uint32_t item) {
    for (auto& o : open) {
        if (o.transfer == transfer && o.item == item) {
            return &o.file;
        }
    }
    return nullptr;
}

files::File& Store::keepOpen(std::deque<Open>& open, uint64_t transfer, uint32_t item, files::File file) {
    open.push_front({transfer, item, std::move(file)});
    while (open.size() > mostOpen) {
        open.pop_back();
    }
    return open.front().file;
}

void Store::close(uint64_t transfer, std::optional<uint32_t> item) {
    for (auto* open : {&readers_, &writers_}) {
        open->erase(std::remove_if(open->begin(), open->end(),
                                   [&](const Open& o) { return o.transfer == transfer && (!item || o.item == *item); }),
                    open->end());
    }
}

// ---- The thread ---------------------------------------------------------------------------

Worker::Worker(Answer answer, std::function<void(const std::string&)> log)
    : answer_(std::move(answer)), thread_([this] { run(); }) {
    store_.log = std::move(log);
}

Worker::~Worker() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
    store_.shutdown();
}

void Worker::post(Request request) {
    {
        std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
    }
    wake_.notify_all();
}

void Worker::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return quit_ || !requests_.empty(); });
        if (quit_) {
            return;
        }
        Request request = std::move(requests_.front());
        requests_.pop_front();
        lock.unlock();
        std::optional<Reply> reply;
        try {
            reply = store_.handle(std::move(request));
        } catch (const std::exception& error) {
            store_.log(error.what());
        }
        if (reply) {
            answer_(std::move(*reply));
        }
        lock.lock();
    }
}

} // namespace rcsdk::file_transfer
