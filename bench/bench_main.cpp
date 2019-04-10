#include "lfqueue/queue.h"
#include "lfqueue/atomic_ops.h"

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>

// -----------------------------------------------------------------------
// Standalone benchmark executable for lockfree-queue.
// Runs multiple throughput and latency tests.
// -----------------------------------------------------------------------

using Clock = std::chrono::high_resolution_clock;

struct alignas(64) BenchConfig {
    std::size_t queue_size = 65536;
    int num_producers = 4;
    int num_consumers = 4;
    int items_per_producer = 2000000;
};

struct alignas(64) BenchStats {
    std::int64_t enqueued{0};
    std::int64_t dequeued{0};
    double elapsed_sec{0.0};
    double enq_ops_per_sec{0.0};
    double deq_ops_per_sec{0.0};
    double p50_latency_ns{0.0};
    double p99_latency_ns{0.0};
    double max_latency_ns{0.0};
};

// -----------------------------------------------------------------------
// Throughput benchmark.
// -----------------------------------------------------------------------
BenchStats run_throughput(const BenchConfig& cfg) {
    lfqueue::MPMCQueue<std::int64_t> q(cfg.queue_size);
    std::atomic<bool> start_flag{false};
    std::atomic<std::int64_t> total_enqueued{0};
    std::atomic<std::int64_t> total_dequeued{0};

    // Consumers.
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
                    while (q.try_dequeue(val)) ++local_count;
                    break;
                } else {
                    lfqueue::detail::cpu_pause();
                }
            }
            total_dequeued.fetch_add(local_count, std::memory_order_relaxed);
        });
    }

    // Producers.
    std::vector<std::thread> producers;
    for (int p = 0; p < cfg.num_producers; ++p) {
        producers.emplace_back([&]() {
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

// -----------------------------------------------------------------------
// Ping-pong latency benchmark (single producer, single consumer).
// Measures round-trip time.
// -----------------------------------------------------------------------
BenchStats run_latency(int iterations = 100000) {
    lfqueue::MPMCQueue<std::int64_t> q(1024);
    std::vector<double> latencies_ns;
    latencies_ns.reserve(iterations);

    std::atomic<bool> ready{false};

    std::thread consumer([&]() {
        while (!ready.load(std::memory_order_acquire)) {
            lfqueue::detail::cpu_pause();
        }

        for (int i = 0; i < iterations; ++i) {
            std::int64_t val;
            while (!q.try_dequeue(val)) {
                lfqueue::detail::cpu_pause();
            }
            // Send it back.
            while (!q.try_enqueue(val)) {
                lfqueue::detail::cpu_pause();
            }
        }
    });

    std::thread producer([&]() {
        ready.store(true, std::memory_order_release);

        for (int i = 0; i < iterations; ++i) {
            auto t0 = Clock::now();
            while (!q.try_enqueue(i)) {
                lfqueue::detail::cpu_pause();
            }
            std::int64_t val;
            while (!q.try_dequeue(val)) {
                lfqueue::detail::cpu_pause();
            }
            auto t1 = Clock::now();
            double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
            latencies_ns.push_back(ns);
        }
    });

    producer.join();
    consumer.join();

    std::sort(latencies_ns.begin(), latencies_ns.end());
    BenchStats stats;
    stats.p50_latency_ns = latencies_ns[latencies_ns.size() / 2];
    stats.p99_latency_ns = latencies_ns[static_cast<std::size_t>(
        latencies_ns.size() * 0.99)];
    stats.max_latency_ns = latencies_ns.back();
    return stats;
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------
int main(int argc, char** argv) {
    fprintf(stdout, "========================================\n");
    fprintf(stdout, "  lockfree-queue benchmark suite\n");
    fprintf(stdout, "========================================\n\n");

    BenchConfig cfg;
    if (argc > 1) cfg.queue_size = static_cast<std::size_t>(std::atol(argv[1]));
    if (argc > 2) cfg.num_producers = std::atoi(argv[2]);
    if (argc > 3) cfg.num_consumers = std::atoi(argv[3]);
    if (argc > 4) cfg.items_per_producer = std::atoi(argv[4]);

    fprintf(stdout, "--- Throughput benchmark ---\n");
    fprintf(stdout, "Queue size: %zu, Producers: %d, Consumers: %d, Items/prod: %d\n\n",
            cfg.queue_size, cfg.num_producers, cfg.num_consumers,
            cfg.items_per_producer);

    // Warmup.
    BenchConfig warmup_cfg = cfg;
    warmup_cfg.items_per_producer = 100000;
    fprintf(stdout, "Warmup...\n");
    run_throughput(warmup_cfg);

    fprintf(stdout, "Running throughput...\n");
    BenchStats tp_stats = run_throughput(cfg);

    fprintf(stdout, "\nThroughput results:\n");
    fprintf(stdout, "  Enqueued:     %lld\n",
            static_cast<long long>(tp_stats.enqueued));
    fprintf(stdout, "  Dequeued:     %lld\n",
            static_cast<long long>(tp_stats.dequeued));
    fprintf(stdout, "  Elapsed:      %.4f sec\n", tp_stats.elapsed_sec);
    fprintf(stdout, "  Enqueue/s:    %.0f\n", tp_stats.enq_ops_per_sec);
    fprintf(stdout, "  Dequeue/s:    %.0f\n", tp_stats.deq_ops_per_sec);
    fprintf(stdout, "  Total ops/s:  %.0f\n\n",
            tp_stats.enq_ops_per_sec + tp_stats.deq_ops_per_sec);

    // Latency benchmark.
    fprintf(stdout, "--- Latency benchmark (ping-pong) ---\n\n");
    BenchStats lat_stats = run_latency(50000);

    fprintf(stdout, "Latency results (round-trip, ns):\n");
    fprintf(stdout, "  P50:  %.0f ns\n", lat_stats.p50_latency_ns);
    fprintf(stdout, "  P99:  %.0f ns\n", lat_stats.p99_latency_ns);
    fprintf(stdout, "  Max:  %.0f ns\n", lat_stats.max_latency_ns);

    fprintf(stdout, "\nDone.\n");
    return 0;
}
