#include "lfqueue/queue.h"
#include "lfqueue/atomic_ops.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cassert>

// Test helpers
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, expr)                                                    \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__);\
            ++tests_failed;                                                 \
        } else {                                                            \
            fprintf(stdout, "PASS: %s\n", name);                            \
            ++tests_passed;                                                 \
        }                                                                   \
    } while(0)

// -----------------------------------------------------------------------
// Single-producer single-consumer basic test
// -----------------------------------------------------------------------
void test_spsc_basic() {
    lfqueue::MPMCQueue<int> q(1024);

    // Enqueue and dequeue a single element.
    TEST("spsc_basic_enqueue", q.try_enqueue(42));
    int val = 0;
    TEST("spsc_basic_dequeue", q.try_dequeue(val));
    TEST("spsc_basic_value", val == 42);

    // Fill and drain.
    for (int i = 0; i < 1000; ++i) {
        TEST("spsc_fill", q.try_enqueue(i));
    }
    for (int i = 0; i < 1000; ++i) {
        int v = 0;
        TEST("spsc_drain", q.try_dequeue(v));
        TEST("spsc_drain_value", v == i);
    }
}

// -----------------------------------------------------------------------
// Queue full / empty behaviour
// -----------------------------------------------------------------------
void test_full_empty() {
    lfqueue::MPMCQueue<int> q(4); // small capacity

    int v = 0;
    TEST("empty_dequeue_fails", !q.try_dequeue(v));

    TEST("fill_1", q.try_enqueue(1));
    TEST("fill_2", q.try_enqueue(2));
    TEST("fill_3", q.try_enqueue(3));
    TEST("fill_4", q.try_enqueue(4));

    // 5th should fail because capacity is 4.
    TEST("full_enqueue_fails", !q.try_enqueue(5));

    TEST("drain_1", q.try_dequeue(v) && v == 1);
    TEST("drain_2", q.try_dequeue(v) && v == 2);
    TEST("drain_3", q.try_dequeue(v) && v == 3);
    TEST("drain_4", q.try_dequeue(v) && v == 4);
    TEST("empty_after_drain", !q.try_dequeue(v));
}

// -----------------------------------------------------------------------
// Multi-producer multi-consumer concurrent test
// -----------------------------------------------------------------------
void test_mpmc_concurrent() {
    constexpr std::size_t kQueueSize = 4096;
    constexpr int kNumProducers = 4;
    constexpr int kNumConsumers = 4;
    constexpr int kItemsPerProducer = 10000;

    lfqueue::MPMCQueue<int> q(kQueueSize);
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> done{false};

    // Consumer threads.
    std::vector<std::thread> consumers;
    for (int c = 0; c < kNumConsumers; ++c) {
        consumers.emplace_back([&]() {
            while (true) {
                int val;
                if (q.try_dequeue(val)) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else if (done.load(std::memory_order_acquire)) {
                    // One more attempt after done flag.
                    if (q.try_dequeue(val)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                } else {
                    lfqueue::detail::cpu_pause();
                }
            }
        });
    }

    // Producer threads.
    std::vector<std::thread> producers;
    for (int p = 0; p < kNumProducers; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < kItemsPerProducer; ++i) {
                while (!q.try_enqueue(p * kItemsPerProducer + i)) {
                    lfqueue::detail::cpu_pause();
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : producers) t.join();
    done.store(true, std::memory_order_release);

    for (auto& t : consumers) t.join();

    int total_produced = produced.load(std::memory_order_relaxed);
    int total_consumed = consumed.load(std::memory_order_relaxed);

    TEST("mpmc_all_produced", total_produced == kNumProducers * kItemsPerProducer);
    TEST("mpmc_all_consumed", total_consumed == total_produced);
}

// -----------------------------------------------------------------------
// Blocking enqueue / dequeue
// -----------------------------------------------------------------------
void test_blocking_ops() {
    lfqueue::MPMCQueue<int> q(256);

    // Fill the queue, then start a thread that blocks on dequeue.
    std::atomic<bool> got_value{false};
    int received = 0;

    std::thread t([&]() {
        received = q.dequeue();
        got_value.store(true, std::memory_order_release);
    });

    // Give the consumer time to block.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TEST("blocking_not_yet", !got_value.load(std::memory_order_acquire));

    q.enqueue(99);
    t.join();

    TEST("blocking_received", got_value.load(std::memory_order_acquire));
    TEST("blocking_value", received == 99);
}

// -----------------------------------------------------------------------
// Size approximation
// -----------------------------------------------------------------------
void test_size_approx() {
    lfqueue::MPMCQueue<int> q(128);

    TEST("size_empty", q.size_approx() == 0);
    q.try_enqueue(1);
    q.try_enqueue(2);
    q.try_enqueue(3);
    TEST("size_after_enqueues", q.size_approx() == 3);

    int v;
    q.try_dequeue(v);
    TEST("size_after_dequeue", q.size_approx() == 2);
}

// -----------------------------------------------------------------------
// Capacity
// -----------------------------------------------------------------------
void test_capacity() {
    lfqueue::MPMCQueue<int> q(1024);
    TEST("capacity", q.capacity() == 1024);
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------
int main() {
    fprintf(stdout, "=== lockfree-queue unit tests ===\n\n");

    test_spsc_basic();
    test_full_empty();
    test_mpmc_concurrent();
    test_blocking_ops();
    test_size_approx();
    test_capacity();

    fprintf(stdout, "\n=== Results: %d passed, %d failed ===\n",
            tests_passed, tests_failed);

    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
