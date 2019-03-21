#ifndef LFQUEUE_HP_H
#define LFQUEUE_HP_H

#include <atomic>
#include <array>
#include <cstdint>
#include <cstddef>
#include <thread>
#include <vector>
#include <cassert>

namespace lfqueue {

// -----------------------------------------------------------------------
// Hazard Pointer (HP) reclamation for lock-free data structures.
//
// Hazard pointers allow safe memory reclamation in lock-free algorithms
// by having each thread "announce" which pointers it is currently
// accessing.  A thread that wants to free a retired pointer must wait
// until no active hazard pointer references it.
//
// This implementation uses a fixed maximum number of hazard pointers
// per thread (kHpPerThread) and a fixed maximum number of retired
// pointers per thread (kRetiredThreshold) before a scan is triggered.
// -----------------------------------------------------------------------

constexpr std::size_t kHpPerThread = 2;           // pointers per thread
constexpr std::size_t kMaxThreads   = 128;         // max tracked threads
constexpr std::size_t kRetiredThreshold = 256;     // trigger scan

// A hazard pointer is simply an atomic pointer that a thread sets
// before accessing a shared object.
struct HazardPointer {
    std::atomic<void*> ptr{nullptr};
};

// Thread-local data for the HP system.
struct alignas(64) HPThreadData {
    std::array<HazardPointer, kHpPerThread> hps;
    std::vector<void*> retired_list;
    std::size_t retired_count = 0;
};

// -----------------------------------------------------------------------
// HazardPointerManager – singleton-like registry.
// -----------------------------------------------------------------------
class HazardPointerManager {
public:
    static HazardPointerManager& instance() noexcept;

    // Register the calling thread and get its thread-local data.
    HPThreadData* register_thread();

    // Unregister the calling thread and reclaim any retired pointers
    // that are safe to free (after scanning).
    void unregister_thread();

    // Retire a pointer for later reclamation.
    void retire(void* ptr);

    // Scan all hazard pointers and free any retired pointer not
    // protected by any HP.
    void scan();

private:
    HazardPointerManager() = default;
    ~HazardPointerManager();

    HazardPointerManager(const HazardPointerManager&) = delete;
    HazardPointerManager& operator=(const HazardPointerManager&) = delete;

    // Find or allocate a slot for the current thread.
    HPThreadData* find_or_alloc_slot();

    // Array of thread data slots.
    std::array<HPThreadData, kMaxThreads> thread_data_;
    std::atomic<std::size_t> num_threads_{0};
};

// -----------------------------------------------------------------------
// RAII helper that sets a hazard pointer and clears on scope exit.
// -----------------------------------------------------------------------
template <typename T>
class HazardPointerGuard {
public:
    HazardPointerGuard(HPThreadData* td, std::size_t hp_idx, T* ptr)
        : td_(td), hp_idx_(hp_idx)
    {
        td_->hps[hp_idx_].ptr.store(ptr, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    ~HazardPointerGuard() {
        td_->hps[hp_idx_].ptr.store(nullptr, std::memory_order_release);
    }

    HazardPointerGuard(const HazardPointerGuard&) = delete;
    HazardPointerGuard& operator=(const HazardPointerGuard&) = delete;

    HazardPointerGuard(HazardPointerGuard&& other) noexcept
        : td_(other.td_), hp_idx_(other.hp_idx_)
    {
        other.td_ = nullptr;
    }

private:
    HPThreadData* td_;
    std::size_t hp_idx_;
};

}  // namespace lfqueue

#endif  // LFQUEUE_HP_H
