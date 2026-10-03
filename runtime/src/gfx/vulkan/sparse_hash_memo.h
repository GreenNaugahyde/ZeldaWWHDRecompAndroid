#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gfxvk {
struct SparseHashStats {
    uint64_t checks = 0, sampleBytes = 0, memoHits = 0, mixerWords = 0, overflows = 0;
};
SparseHashStats sparse_hash_stats();
struct SparseSampleCounts { uint64_t words, bytes; };
inline SparseSampleCounts sparse_sample_counts(size_t size) {
    size_t step = std::max<size_t>((size / 256) & ~size_t(7), 8);
    size_t full = size >= 8 ? (size - 8) / step + 1 : 0;
    size_t offset = full * step;
    size_t tail = offset < size ? size - offset : 0;
    return {uint64_t(full) + (tail != 0), uint64_t(full) * 8 + tail};
}

// Exactly the original sample offsets, including its zero-padded short tail.
template<class Consume> void sparse_sample_words(const uint8_t* bytes, size_t size, Consume consume) {
    size_t step = std::max<size_t>((size / 256) & ~size_t(7), 8), offset = 0;
    for(; offset < size && size - offset >= 8; offset += step) {
        uint64_t value;
        memcpy(&value, bytes + offset, 8);
        consume(value, size_t(8));
    }
    if(offset < size) {
        uint64_t value = 0;
        memcpy(&value, bytes + offset, size - offset);
        consume(value, size - offset);
    }
}
// Render-thread-only, bounded storage. Identity selects a slot; every hit still
// requires exact equality of all freshly read ordered samples and their count.
template<size_t Entries = 64, size_t MaxSamples = 8192> class SparseHashMemo {
    static_assert(Entries && MaxSamples);
    struct Entry {
        uintptr_t identity = 0;
        size_t count = 0;
        uint64_t hash = 0;
        bool valid = false;
    };
    // Keep identity probes contiguous rather than 64KiB apart in sample storage.
    std::array<Entry, Entries> entries;
    std::array<std::array<uint64_t, MaxSamples>, Entries> snapshots;
    std::array<uint64_t, MaxSamples> scratch;
    size_t count = 0, next = 0, recent = 0;
    bool overflow = false;
    uint64_t streamed = basis;
    static uint64_t mix(uint64_t hash, uint64_t word) { return (hash ^ word) * 0x100000001b3ull; }
public:
    static constexpr uint64_t basis = 0xcbf29ce484222325ull;
    void begin() { count = 0; overflow = false; streamed = basis; }
    void add(uint64_t word) {
        if(!overflow && count < MaxSamples) { scratch[count++] = word; return; }
        if(!overflow) {
            for(size_t i = 0; i < count; ++i) streamed = mix(streamed, scratch[i]);
            overflow = true;
        }
        streamed = mix(streamed, word);
        ++count;
    }
    uint64_t finish(uintptr_t identity, SparseHashStats& stats) {
        if(overflow) { ++stats.overflows; stats.mixerWords += count; return streamed; }
        Entry* selected = nullptr;
        size_t index = recent;
        for(size_t offset = 0; offset < Entries; ++offset) {
            index = (recent + offset) % Entries;
            auto& entry = entries[index];
            if(entry.valid && entry.identity == identity) { selected = &entry; break; }
        }
        if(selected && selected->count == count &&
           (!count || !memcmp(snapshots[index].data(), scratch.data(), count * sizeof(uint64_t)))) {
            recent = index;
            ++stats.memoHits;
            return selected->hash;
        }
        uint64_t hash = basis;
        for(size_t i = 0; i < count; ++i) hash = mix(hash, scratch[i]);
        stats.mixerWords += count;
        if(!selected) {
            index = next; selected = &entries[index]; next = (next + 1) % Entries;
        }
        if(count) memcpy(snapshots[index].data(), scratch.data(), count * sizeof(uint64_t));
        selected->identity = identity; selected->count = count;
        selected->hash = hash; selected->valid = true; recent = index;
        return hash;
    }
};
} // namespace gfxvk
