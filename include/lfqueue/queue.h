#ifndef LFQUEUE_QUEUE_H
#define LFQUEUE_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <type_traits>
#include <new>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>   // SSE2
#include <smmintrin.h>   // SSE4.1
#endif

namespace lfqueue {

// -----------------------------------------------------------------------
// Cache line alignment helpers
// -----------------------------------------------------------------------
constexpr std::size_t kCacheLineSize = 64;

template <typename T>
struct alignas(kCacheLineSize) CacheAligned {
    T value;
};

// -----------------------------------------------------------------------
// Compile-time power-of-two check and log2
// -----------------------------------------------------------------------
constexpr bool is_pow2(std::size_t v) noexcept {
    return v && !(v & (v - 1));
}

constexpr std::size_t log2_pow2(std::size_t v) noexcept {
    std::size_t n = 0;
    while (v >>= 1) ++n;
    return n;
}

// -----------------------------------------------------------------------
// Sequence number type – 64-bit monotonic counters
// -----------------------------------------------------------------------
using seq_t = std::uint64_t;

// -----------------------------------------------------------------------
// Slot within the ring buffer
// -----------------------------------------------------------------------
template <typename T>
struct alignas(kCacheLineSize) Slot {
    // The sequence number acts as both an availability marker and a
    // generation counter to solve the ABA problem.
    std::atomic<seq_t> seq;
    // Padding between seq and data to avoid false sharing between
    // producer and consumer touching different fields.
    char padding[kCacheLineSize - sizeof(std::atomic<seq_t>)];
    // The actual stored element.  We keep it uninitialised until
    // placement-new is called.
    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;

    Slot() noexcept : seq(0) {}

    T* ptr() noexcept {
        return reinterpret_cast<T*>(&storage);
    }

    const T* ptr() const noexcept {
        return reinterpret_cast<const T*>(&storage);
    }
};

// -----------------------------------------------------------------------
// Lock-free MPMC bounded queue (ring-buffer based).
//
// Based on the algorithm by Vyukov:
//   https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc
//
// The queue uses a fixed-size ring buffer where each slot has a sequence
// counter.  Producers claim a slot by atomically incrementing the enqueue
// index and then waiting until the slot's sequence matches the expected
// value.  Consumers do the symmetric operation on the dequeue index.
// -----------------------------------------------------------------------
template <typename T>
class MPMCQueue {
public:
    // buffer_size must be a power of two.
    explicit MPMCQueue(std::size_t buffer_size);
    ~MPMCQueue();

    // Non-copyable, non-movable.
    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;
    MPMCQueue(MPMCQueue&&) = delete;
    MPMCQueue& operator=(MPMCQueue&&) = delete;

    // Enqueue one element.  Returns true on success, false when full.
    template <typename U>
    bool try_enqueue(U&& item);

    // Blocking enqueue – spins until a slot becomes available.
    template <typename U>
    void enqueue(U&& item);

    // Dequeue one element into `item`.  Returns true on success.
    bool try_dequeue(T& item);

    // Blocking dequeue – spins until an element is available.
    T dequeue();

    // Current approximate number of elements.  Only suitable for
    // monitoring / debugging – not synchronisation.
    std::size_t size_approx() const noexcept;

    // Capacity (fixed at construction).
    std::size_t capacity() const noexcept { return capacity_; }

    // Mask for fast modulo.
    std::size_t mask() const noexcept { return mask_; }

private:
    const std::size_t capacity_;
    const std::size_t mask_;

    // Producer index – only written by producers, read by consumers.
    alignas(kCacheLineSize) std::atomic<seq_t> enqueue_pos_{0};

    // Consumer index – only written by consumers, read by producers.
    alignas(kCacheLineSize) std::atomic<seq_t> dequeue_pos_{0};

    // Ring buffer of slots.
    Slot<T>* slots_;
};

// -----------------------------------------------------------------------
// Implementation – in header because of the template.
// -----------------------------------------------------------------------

template <typename T>
MPMCQueue<T>::MPMCQueue(std::size_t buffer_size)
    : capacity_(buffer_size)
    , mask_(buffer_size - 1)
{
    if (!is_pow2(buffer_size) || buffer_size < 2) {
        throw std::invalid_argument(
            "MPMCQueue: buffer_size must be a power of two and >= 2");
    }
    slots_ = static_cast<Slot<T>*>(
        std::aligned_alloc(kCacheLineSize, sizeof(Slot<T>) * buffer_size));
    if (!slots_) {
        throw std::bad_alloc();
    }
    // Initialise every slot's sequence to its index so that producers
    // start by claiming slot 0.
    for (std::size_t i = 0; i < buffer_size; ++i) {
        new (&slots_[i]) Slot<T>();
        slots_[i].seq.store(i, std::memory_order_relaxed);
    }
}

template <typename T>
MPMCQueue<T>::~MPMCQueue() {
    // Destroy any elements still in the queue.
    for (std::size_t i = 0; i < capacity_; ++i) {
        seq_t seq = slots_[i].seq.load(std::memory_order_relaxed);
        // If the slot has been claimed by a producer but not yet consumed,
        // we need to destroy the object.
        // seq > i + capacity_ means it's been produced but not consumed.
        if (seq > i + capacity_) {
            slots_[i].ptr()->~T();
        }
        slots_[i].~Slot<T>();
    }
    std::free(slots_);
}

template <typename T>
template <typename U>
bool MPMCQueue<T>::try_enqueue(U&& item) {
    seq_t pos;
    Slot<T>* slot;
    seq_t slot_seq;

    // Fast-path: try to claim a slot.
    while (true) {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
        slot = &slots_[pos & mask_];
        slot_seq = slot->seq.load(std::memory_order_acquire);

        // Expected sequence for an empty slot ready to be written.
        seq_t expected = pos - capacity_;
        if (slot_seq == expected) {
            // We can claim this slot.  CAS the enqueue position forward.
            if (enqueue_pos_.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed)) {
                break;  // slot claimed
            }
            // CAS failed – someone else claimed it, retry.
        } else if (slot_seq < expected) {
            // Queue is full – slot_seq is behind, meaning no slot
            // has been consumed yet.
            return false;
        } else {
            // slot_seq > expected – another producer is still
            // writing to an earlier slot; we could spin but for
            // try_enqueue we treat this as contention and retry.
            // To avoid busy-spinning we yield once.
            #if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
            #endif
        }
    }

    // Write the element into the claimed slot.
    ::new (slot->ptr()) T(std::forward<U>(item));

    // Release the slot to consumers by writing the next expected
    // consumer sequence.
    slot->seq.store(pos + 1, std::memory_order_release);
    return true;
}

template <typename T>
template <typename U>
void MPMCQueue<T>::enqueue(U&& item) {
    seq_t pos;
    Slot<T>* slot;
    seq_t slot_seq;

    while (true) {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
        slot = &slots_[pos & mask_];
        slot_seq = slot->seq.load(std::memory_order_acquire);

        seq_t expected = pos - capacity_;
        if (slot_seq == expected) {
            if (enqueue_pos_.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed)) {
                break;
            }
        } else {
            // Busy-wait with pause.
            #if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
            #endif
        }
    }

    ::new (slot->ptr()) T(std::forward<U>(item));
    slot->seq.store(pos + 1, std::memory_order_release);
}

template <typename T>
bool MPMCQueue<T>::try_dequeue(T& item) {
    seq_t pos;
    Slot<T>* slot;
    seq_t slot_seq;

    while (true) {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
        slot = &slots_[pos & mask_];
        slot_seq = slot->seq.load(std::memory_order_acquire);

        // Expected sequence for a filled slot.
        seq_t expected = pos + 1;
        if (slot_seq == expected) {
            if (dequeue_pos_.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed)) {
                break;
            }
        } else if (slot_seq < expected) {
            // Queue is empty.
            return false;
        } else {
            // slot_seq > expected – a producer has written but not
            // yet released (shouldn't happen with correct release
            // ordering, but pause and retry).
            #if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
            #endif
        }
    }

    // Read the element.
    item = std::move(*slot->ptr());
    // Destroy the element in the slot.
    slot->ptr()->~T();

    // Release the slot back to producers.
    slot->seq.store(pos + capacity_ + 1, std::memory_order_release);
    return true;
}

template <typename T>
T MPMCQueue<T>::dequeue() {
    T item;
    while (true) {
        seq_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Slot<T>* slot = &slots_[pos & mask_];
        seq_t slot_seq = slot->seq.load(std::memory_order_acquire);

        seq_t expected = pos + 1;
        if (slot_seq == expected) {
            if (dequeue_pos_.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed)) {
                item = std::move(*slot->ptr());
                slot->ptr()->~T();
                slot->seq.store(pos + capacity_ + 1, std::memory_order_release);
                return item;
            }
        } else {
            #if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
            #endif
        }
    }
}

template <typename T>
std::size_t MPMCQueue<T>::size_approx() const noexcept {
    seq_t enq = enqueue_pos_.load(std::memory_order_relaxed);
    seq_t deq = dequeue_pos_.load(std::memory_order_relaxed);
    // enq - deq can be negative under relaxed ordering, so we clamp.
    seq_t diff = enq - deq;
    return static_cast<std::size_t>(
        diff < 0 ? 0 : (diff > static_cast<seq_t>(capacity_) ? capacity_ : diff));
}

}  // namespace lfqueue

#endif  // LFQUEUE_QUEUE_H
