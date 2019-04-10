#include "lfqueue/queue.h"
#include "lfqueue/atomic_ops.h"

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>

// -----------------------------------------------------------------------
// Micro-benchmark: measure throughput of MPMCQueue under contention.
// -----------------------------------------------------------------------

using Clock = std::chrono::high_resolution_clock;

struct alignas(64) BenchConfig {
    std::size_t queue_size = 65536;
    int num_producers = 4;
    int num_consumers = 4;
    int items_per_producer = 500000;
};

struct alignas(64) BenchStats {
    std::int64_t enqueued{0};
    std::int64_t dequeued{0};
    double elapsed_sec{0.0};
    double enq_ops_per_sec{0.0};
    double deq_ops_per_sec{0.0};
};

static BenchStats run_benchmark(const BenchConfig& cfg) {
    lfqueue::MPMCQueue<std::int64_t> q(cfg.queue_size);
    std::atomic<bool> start_flag{false};
    std::atomic<std::int64_t> total_enqueued{0};
    std::atomic<std::int64_t> total_dequeued{0};

    // Consumer threads.
    std::vector<std::thread> consumers;
    std::atomic<int> consumers_done{0};
    for (int c = 0; c < cfg.num_consumers; ++c) {
        consumers.emplace_back([&]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                lfqueue::detail::cpu_pause();
            }

            std::int64_t local_count = 0;
            std::int64_t val;

            while (true) {
                if (q.try_dequeue(val)) {
                    ++local_count;
                } else if (consumers_done.load(std::memory_order_relaxed) >=
                           cfg.num_producers) {
                    // One last attempt.
                    while (q.try_dequeue(val)) {
                        ++local_count;
                    }
                    break;
                } else {
                    lfqueue::detail::cpu_pause();
                }
            }

            total_dequeued.fetch_add(local_count, std::memory_order_relaxed);
        });
    }

    // Producer threads.
    std::vector<std::thread> producers;
    for (int p = 0; p < cfg.num_producers; ++p) {
        producers.emplace_back([&, p]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                lfqueue::detail::cpu_pause();
            }

            std::int64_t local_count = 0;
            for (int i = 0; i < cfg.items_per_producer; ++i) {
                while (!q.try_enqueue(i)) {
                    lfqueue::detail::cpu_pause();
                }
                ++local_count;
            }

            total_enqueued.fetch_add(local_count, std::memory_order_relaxed);
            consumers_done.fetch_add(1, std::memory_order_release);
        });
    }

    // Start.
    auto t0 = Clock::now();
    start_flag.store(true, std::memory_order_release);

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    auto t1 = Clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    BenchStats stats;
    stats.enqueued = total_enqueued.load(std::memory_order_relaxed);
    stats.dequeued = total_dequeued.load(std::memory_order_relaxed);
    stats.elapsed_sec = sec;
    stats.enq_ops_per_sec = static_cast<double>(stats.enqueued) / sec;
    stats.deq_ops_per_sec = static_cast<double>(stats.dequeued) / sec;

    return stats;
}

int main() {
    fprintf(stdout, "=== lockfree-queue benchmark ===\n\n");

    BenchConfig cfg;
    cfg.queue_size = 65536;
    cfg.num_producers = 4;
    cfg.num_consumers = 4;
    cfg.items_per_producer = 2000000;  // 8M total items

    fprintf(stdout, "Configuration:\n");
    fprintf(stdout, "  Queue size:       %zu\n", cfg.queue_size);
    fprintf(stdout, "  Producers:        %d\n", cfg.num_producers);
    fprintf(stdout, "  Consumers:        %d\n", cfg.num_consumers);
    fprintf(stdout, "  Items/producer:   %d\n", cfg.items_per_producer);
    fprintf(stdout, "  Total items:      %d\n\n",
            cfg.num_producers * cfg.items_per_producer);

    // Warmup run.
    fprintf(stdout, "Warmup...\n");
    run_benchmark(cfg);

    // Measured run.
    fprintf(stdout, "Running...\n");
    BenchStats stats = run_benchmark(cfg);

    fprintf(stdout, "\nResults:\n");
    fprintf(stdout, "  Enqueued:         %lld\n",
            static_cast<long long>(stats.enqueued));
    fprintf(stdout, "  Dequeued:         %lld\n",
            static_cast<long long>(stats.dequeued));
    fprintf(stdout, "  Elapsed:          %.4f sec\n", stats.elapsed_sec);
    fprintf(stdout, "  Enqueue ops/sec:  %.0f\n", stats.enq_ops_per_sec);
    fprintf(stdout, "  Dequeue ops/sec:  %.0f\n", stats.deq_ops_per_sec);
    fprintf(stdout, "  Total ops/sec:    %.0f\n",
            stats.enq_ops_per_sec + stats.deq_ops_per_sec);

    // Sanity check.
    if (stats.enqueued == stats.dequeued) {
        fprintf(stdout, "\n✓ All items accounted for.\n");
        return 0;
    } else {
        fprintf(stderr, "\n✗ Mismatch: enqueued=%lld dequeued=%lld\n",
                static_cast<long long>(stats.enqueued),
                static_cast<long long>(stats.dequeued));
        return 1;
    }
}
