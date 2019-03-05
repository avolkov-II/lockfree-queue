# lockfree-queue

**A lock-free multi-producer multi-consumer (MPMC) bounded queue in C++ for low-latency inter-thread communication.**

Implements Dmitry Vyukov's bounded MPMC ring-buffer algorithm with cache-line padding, SSE4.1 pause hints, and hazard pointer memory reclamation. Built for trading systems and other latency-sensitive applications where every nanosecond counts.

## Features

- **Lock-free** — no mutexes, no condition variables, no syscalls in the hot path.
- **MPMC** — any number of threads can enqueue and dequeue concurrently.
- **Bounded** — fixed capacity allocated at construction (power-of-two).
- **Cache-line aligned** — slots and indices are padded to 64 bytes to prevent false sharing.
- **SIMD pause hints** — `_mm_pause()` on x86 for spin-wait efficiency.
- **Hazard pointer (HP) reclamation** — safe memory management for lock-free structures (included but optional).
- **Blocking and non-blocking APIs** — `try_enqueue`/`try_dequeue` for non-blocking use, `enqueue`/`dequeue` for blocking (spin-wait).

## Tech Stack

- **Language:** C++17 (with some C++20 `std::atomic` features)
- **Build:** CMake 3.14+
- **Architecture:** x86-64 (SSE2/SSE4.1) with ARM64 fallback
- **Testing:** Custom test harness (no external dependencies)

## Prerequisites

- CMake ≥ 3.14
- A C++17-compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- Linux, macOS, or Windows

## Installation

```bash
git clone https://github.com/avolkov-II/lockfree-queue.git
cd lockfree-queue
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### Build Options

| Option                   | Default | Description                        |
|--------------------------|---------|------------------------------------|
| `LFQUEUE_BUILD_TESTS`    | ON      | Build unit tests                   |
| `LFQUEUE_BUILD_BENCH`    | ON      | Build benchmark executable         |

Example with options:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DLFQUEUE_BUILD_TESTS=OFF
```

## Usage

### Basic Example

```cpp
#include "lfqueue/queue.h"
#include <thread>

lfqueue::MPMCQueue<int> q(1024);  // power-of-two capacity

// Producer thread
std::thread producer([&]() {
    for (int i = 0; i < 1000; ++i) {
        q.enqueue(i);                // blocking
        // or: while (!q.try_enqueue(i)) { _mm_pause(); }
    }
});

// Consumer thread
std::thread consumer([&]() {
    for (int i = 0; i < 1000; ++i) {
        int val = q.dequeue();       // blocking
        // or:
        // int val;
        // while (!q.try_dequeue(val)) { _mm_pause(); }
    }
});

producer.join();
consumer.join();
```

### Non-Blocking API

```cpp
lfqueue::MPMCQueue<double> q(256);

if (q.try_enqueue(3.14159)) {
    // slot claimed and written
}

double val;
if (q.try_dequeue(val)) {
    // val contains the dequeued element
}
```

### Monitoring

```cpp
std::size_t approx_size = q.size_approx();  // approximate fill level
std::size_t cap = q.capacity();             // fixed capacity
```

## API Reference

### `MPMCQueue<T>(std::size_t buffer_size)`

Constructs a queue with `buffer_size` slots. Must be a power of two ≥ 2.

| Method                              | Description                                    |
|-------------------------------------|------------------------------------------------|
| `bool try_enqueue(U&& item)`        | Enqueue if slot available. Returns false if full. |
| `void enqueue(U&& item)`            | Enqueue, spinning until a slot is available.   |
| `bool try_dequeue(T& item)`         | Dequeue into `item`. Returns false if empty.   |
| `T dequeue()`                       | Dequeue, spinning until an element is available. |
| `std::size_t size_approx() const`   | Approximate number of elements (debug/monitoring). |
| `std::size_t capacity() const`      | Fixed capacity.                                |

## Architecture

```
                   enqueue_pos_ (atomic, padded)
                  ┌──────────────────────┐
                  │        enqueue       │
                  │          │            │
                  ▼          ▼            ▼
        ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┐
        │  0  │  1  │  2  │  3  │ ... │ N-2 │ N-1 │  ← ring buffer
        └─────┴─────┴─────┴─────┴─────┴─────┴─────┘
                  ▲          ▲
                  │          │
                  └─ dequeue ─┘
                  dequeue_pos_ (atomic, padded)
```

Each slot contains a 64-bit sequence counter that acts as both a generation number (ABA prevention) and a state indicator. Producers atomically CAS the enqueue index, then wait for the slot's sequence to match `pos - capacity`. Consumers atomically CAS the dequeue index, then wait for `pos + 1`.

## Running Tests

```bash
cd build
ctest --output-on-failure
```

Or run directly:

```bash
./lfqueue_test
```

## Benchmarks

```bash
cd build
./lfqueue_bench [queue_size] [num_producers] [num_consumers] [items_per_producer]
```

Example output (4 producers, 4 consumers, 8M items on an Intel Xeon Gold 6138):

```
Throughput results:
  Enqueued:     8000000
  Dequeued:     8000000
  Elapsed:      0.2841 sec
  Enqueue/s:    28158310
  Dequeue/s:    28158310
  Total ops/s:  56316620
```

## License

MIT — see [LICENSE](LICENSE).

## References

- [Vyukov, D. "Bounded MPMC queue"](https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc)
- [Maged Michael, "Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects"](https://www.research.ibm.com/people/m/michael/ieeetpds-2004.pdf)
