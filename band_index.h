#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

// Exact Hamming candidate index for thresholds below the band count. With at
// most 15 changed bits across 16 bands, at least one 16-bit band is unchanged.
// Candidates are still verified with the full 256-bit Hamming distance.
template <typename HashT, size_t BandCount = 16>
class HashBandIndex {
public:
    static constexpr int kMaximumExactDistance = int(BandCount) - 1;

    // Each worker owns complete sorted bands, so construction is parallel,
    // lock-free, compact, and cache-friendly.
    void build(const std::vector<HashT>& hashes, int workerCount) {
        hashes_=hashes;
        for (auto& band:bands_) {
            band.clear();
            band.reserve(hashes.size());
        }
        std::atomic<size_t> nextBand{0};
        const int workers=std::clamp(workerCount,1,int(BandCount));
        std::vector<std::thread> threads;
        threads.reserve(size_t(workers));
        for (int worker=0;worker<workers;++worker) threads.emplace_back([&] {
            while (true) {
                const size_t band=nextBand.fetch_add(1);
                if (band>=BandCount) break;
                auto& index=bands_[band];
                for (int image=0;image<int(hashes_.size());++image)
                    index.push_back(Entry{bandValue(hashes_[size_t(image)],band),image});
                std::sort(index.begin(),index.end(),[](const Entry& left,const Entry& right) {
                    return left.value<right.value
                        || (left.value==right.value && left.index<right.index);
                });
            }
        });
        for (auto& thread:threads) thread.join();
    }

    std::vector<int> query(const HashT& hash, int maxDistance,
                           int exclude = -1) const {
        if (maxDistance > kMaximumExactDistance) return {};
        std::vector<int> candidates;
        for (size_t band = 0; band < BandCount; ++band) {
            const uint16_t value=bandValue(hash,band);
            auto found=std::lower_bound(bands_[band].begin(),bands_[band].end(),value,
                [](const Entry& entry,uint16_t target){return entry.value<target;});
            for (;found!=bands_[band].end() && found->value==value;++found)
                candidates.push_back(found->index);
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
            [&](int index) {
                return index == exclude || index < 0 || size_t(index) >= hashes_.size()
                    || metric(hash, hashes_[size_t(index)]) > maxDistance;
            }), candidates.end());
        return candidates;
    }

private:
    static_assert(HashT().size() == BandCount * 16,
                  "HashBandIndex expects 16-bit bands");

    struct Entry {
        uint16_t value{};
        int index{};
    };

    static uint16_t bandValue(const HashT& hash, size_t band) {
        uint16_t value = 0;
        const size_t first = band * 16;
        for (size_t bit = 0; bit < 16; ++bit)
            if (hash.test(first + bit)) value |= uint16_t(1) << bit;
        return value;
    }

    static int metric(const HashT& left, const HashT& right) {
        return int((left ^ right).count());
    }

    std::array<std::vector<Entry>, BandCount> bands_;
    std::vector<HashT> hashes_;
};
