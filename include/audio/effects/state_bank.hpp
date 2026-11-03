#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace audiox::effects {

// Builtin graph nodes are stereo, with at most 64 nodes. Evict only the least
// recently processed inactive identity rather than accumulating deleted effects.
template <typename State> class StateBank {
    struct Entry {
        char id[64]{};
        uint8_t channel{};
        uint32_t hash{};
        uint64_t used{};
        State state{};
    };
    Entry entries_[128]{};
    uint64_t sequence_{};

  public:
    State *find(const char *id, uint8_t channel, bool create = true) {
        const char *name = id ? id : "fx";
        uint32_t hash = 2166136261U ^ channel;
        for (const char *p = name; *p; ++p)
            hash = (hash ^ static_cast<unsigned char>(*p)) * 16777619U;
        Entry *oldest = &entries_[0];
        for (auto &entry : entries_) {
            if (entry.used && entry.hash == hash && entry.channel == channel &&
                !strcmp(entry.id, name)) {
                entry.used = ++sequence_;
                return &entry.state;
            }
            if (entry.used < oldest->used)
                oldest = &entry;
        }
        if (!create)
            return nullptr;
        oldest->state = {};
        snprintf(oldest->id, sizeof(oldest->id), "%s", name);
        oldest->channel = channel;
        oldest->hash = hash;
        oldest->used = ++sequence_;
        return &oldest->state;
    }
};

} // namespace audiox::effects
