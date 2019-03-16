#ifndef LFQUEUE_ATOMIC_OPS_H
#define LFQUEUE_ATOMIC_OPS_H

#include <atomic>
#include <cstdint>
#include <type_traits>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#include <immintrin.h>
#endif

namespace lfqueue {
namespace detail {

// -----------------------------------------------------------------------
// Compiler fence – prevents compiler reordering without a hardware fence.
// -----------------------------------------------------------------------
inline void compiler_fence() noexcept {
    asm volatile("" ::: "memory");
}

// -----------------------------------------------------------------------
// Hardware memory barrier (full fence).
// -----------------------------------------------------------------------
inline void mfence() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

// -----------------------------------------------------------------------
// Pause / yield hint for spin-wait loops.
// -----------------------------------------------------------------------
inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// -----------------------------------------------------------------------
// Prefetch a cache line for read (low temporal locality hint).
// -----------------------------------------------------------------------
inline void prefetch_read(const void* addr) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0);
#else
    (void)addr;
#endif
}

// -----------------------------------------------------------------------
// Prefetch a cache line for write.
// -----------------------------------------------------------------------
inline void prefetch_write(const void* addr) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    // Use SSE4.1 _mm_prefetch with _MM_HINT_ET0 (write prefetch)
    // Not all compilers expose this, so fall back to T0.
    _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0);
#else
    (void)addr;
#endif
}

// -----------------------------------------------------------------------
// Atomic load with memory ordering.
// -----------------------------------------------------------------------
template <typename T>
inline T atomic_load(const std::atomic<T>& atom,
                     std::memory_order order = std::memory_order_acquire) noexcept {
    return atom.load(order);
}

// -----------------------------------------------------------------------
// Atomic store with memory ordering.
// -----------------------------------------------------------------------
template <typename T>
inline void atomic_store(std::atomic<T>& atom, T value,
                         std::memory_order order = std::memory_order_release) noexcept {
    atom.store(value, order);
}

// -----------------------------------------------------------------------
// Relaxed load (fast path).
// -----------------------------------------------------------------------
template <typename T>
inline T relaxed_load(const std::atomic<T>& atom) noexcept {
    return atom.load(std::memory_order_relaxed);
}

// -----------------------------------------------------------------------
// Relaxed store.
// -----------------------------------------------------------------------
template <typename T>
inline void relaxed_store(std::atomic<T>& atom, T value) noexcept {
    atom.store(value, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------
// Strong CAS with acquire-release semantics.
// -----------------------------------------------------------------------
template <typename T>
inline bool cas(std::atomic<T>& atom, T& expected, T desired) noexcept {
    return atom.compare_exchange_strong(expected, desired,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
}

// -----------------------------------------------------------------------
// Weak CAS (may spuriously fail – slightly faster on some architectures).
// -----------------------------------------------------------------------
template <typename T>
inline bool cas_weak(std::atomic<T>& atom, T& expected, T desired) noexcept {
    return atom.compare_exchange_weak(expected, desired,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire);
}

// -----------------------------------------------------------------------
// Fetch-and-add with acquire-release.
// -----------------------------------------------------------------------
template <typename T>
inline T fetch_add(std::atomic<T>& atom, T delta) noexcept {
    return atom.fetch_add(delta, std::memory_order_acq_rel);
}

// -----------------------------------------------------------------------
// Cache-line flush (for persistence / cross-socket visibility).
// -----------------------------------------------------------------------
inline void clflush(const void* addr) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_clflush(addr);
#else
    (void)addr;
#endif
}

// -----------------------------------------------------------------------
// Cache-line flush with write-back (clwb on Intel, or dczva on ARM).
// -----------------------------------------------------------------------
inline void clwb(const void* addr) noexcept {
#if defined(__x86_64__) && defined(__CLWB__)
    _mm_clwb(addr);
#else
    // Fallback: clflush is safe but slower.
    clflush(addr);
#endif
}

}  // namespace detail
}  // namespace lfqueue

#endif  // LFQUEUE_ATOMIC_OPS_H
