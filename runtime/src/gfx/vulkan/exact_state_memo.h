#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gfxvk::vk {
// Render-thread-only: callers must freshly gather every relevant word first.
template<size_t MaxWords> class ExactStateMemo {
    struct Entry {
        std::array<uint32_t, MaxWords> words;
        size_t count = 0;
        uint64_t seed = 0, hash = 0;
        bool valid = false;
    };
    struct Stage {
        std::array<Entry, 4> entries;
        size_t next = 0, recent = 0;
    };
    std::array<Stage, 2> stages;
public:
    void reset() {
        for(auto& stage : stages) {
            for(auto& entry : stage.entries) entry.valid = false;
            stage.next = stage.recent = 0;
        }
    }
    bool find(bool vertex, const uint32_t* words, size_t count, uint64_t seed, uint64_t& hash) {
        if(count > MaxWords) return false;
        auto& stage = stages[vertex ? 1 : 0];
        for(size_t offset = 0; offset < 4; ++offset) {
            size_t index = (stage.recent + offset) % 4;
            const auto& entry = stage.entries[index];
            if(entry.valid && entry.count == count && entry.seed == seed &&
               (!count || !memcmp(entry.words.data(), words, count * sizeof(uint32_t)))) {
                stage.recent = index;
                hash = entry.hash;
                return true;
            }
        }
        return false;
    }
    void remember(bool vertex, const uint32_t* words, size_t count, uint64_t seed, uint64_t hash) {
        if(count > MaxWords) return;
        auto& stage = stages[vertex ? 1 : 0];
        auto& entry = stage.entries[stage.next];
        if(count) memcpy(entry.words.data(), words, count * sizeof(uint32_t));
        entry.count = count; entry.seed = seed; entry.hash = hash; entry.valid = true;
        stage.recent = stage.next;
        stage.next = (stage.next + 1) % 4;
    }
};
} // namespace gfxvk::vk
