#include "lfqueue/hp.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace lfqueue {

// -----------------------------------------------------------------------
// HazardPointerManager implementation
// -----------------------------------------------------------------------

HazardPointerManager& HazardPointerManager::instance() noexcept {
    static HazardPointerManager mgr;
    return mgr;
}

HPThreadData* HazardPointerManager::register_thread() {
    HPThreadData* td = find_or_alloc_slot();
    // Ensure retired list is empty for a fresh registration.
    td->retired_list.clear();
    td->retired_count = 0;
    return td;
}

void HazardPointerManager::unregister_thread() {
    HPThreadData* td = find_or_alloc_slot();
    // Reclaim any pointers that are still in our retired list.
    for (void* ptr : td->retired_list) {
        std::free(ptr);
    }
    td->retired_list.clear();
    td->retired_count = 0;

    // Clear all hazard pointers for this thread.
    for (auto& hp : td->hps) {
        hp.ptr.store(nullptr, std::memory_order_relaxed);
    }
}

void HazardPointerManager::retire(void* ptr) {
    HPThreadData* td = find_or_alloc_slot();
    td->retired_list.push_back(ptr);
    td->retired_count++;

    if (td->retired_count >= kRetiredThreshold) {
        scan();
    }
}

void HazardPointerManager::scan() {
    HPThreadData* td = find_or_alloc_slot();
    if (td->retired_list.empty()) return;

    // Collect all active hazard pointers from all threads.
    std::size_t num_hps = 0;
    void* hp_values[kMaxThreads * kHpPerThread];

    std::size_t n = num_threads_.load(std::memory_order_acquire);
    for (std::size_t t = 0; t < n && t < kMaxThreads; ++t) {
        for (std::size_t h = 0; h < kHpPerThread; ++h) {
            void* val = thread_data_[t].hps[h].ptr.load(std::memory_order_acquire);
            if (val != nullptr) {
                hp_values[num_hps++] = val;
            }
        }
    }

    // Sort the hazard pointer values for binary search.
    std::sort(hp_values, hp_values + num_hps);

    // Partition retired list: keep only pointers not in hp_values.
    auto it = std::partition(td->retired_list.begin(), td->retired_list.end(),
        [&](void* ptr) {
            return std::binary_search(hp_values, hp_values + num_hps, ptr);
        });

    // Free the pointers that are safe to reclaim.
    for (auto free_it = it; free_it != td->retired_list.end(); ++free_it) {
        std::free(*free_it);
    }

    // Erase the freed pointers.
    td->retired_list.erase(it, td->retired_list.end());
    td->retired_count = td->retired_list.size();
}

HazardPointerManager::~HazardPointerManager() {
    // Free any remaining retired pointers across all slots.
    for (std::size_t t = 0; t < kMaxThreads; ++t) {
        for (void* ptr : thread_data_[t].retired_list) {
            std::free(ptr);
        }
        thread_data_[t].retired_list.clear();
        thread_data_[t].retired_count = 0;
    }
}

HPThreadData* HazardPointerManager::find_or_alloc_slot() {
    // We use a simple thread-local cache to avoid repeated lookups.
    static thread_local HPThreadData* cached_td = nullptr;
    if (cached_td) return cached_td;

    std::size_t idx = num_threads_.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= kMaxThreads) {
        // We've exhausted slots – fall back to slot 0 (last resort).
        // This should never happen in practice with reasonable thread counts.
        idx = 0;
    }
    cached_td = &thread_data_[idx];
    return cached_td;
}

}  // namespace lfqueue
