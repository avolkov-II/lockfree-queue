#include "lfqueue/hp.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <atomic>

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
// Test that hazard pointer registration works.
// -----------------------------------------------------------------------
void test_register_unregister() {
    auto& mgr = lfqueue::HazardPointerManager::instance();
    lfqueue::HPThreadData* td = mgr.register_thread();
    TEST("register_not_null", td != nullptr);
    TEST("register_hps_array", td->hps.size() == lfqueue::kHpPerThread);
    TEST("register_retired_empty", td->retired_list.empty());
    mgr.unregister_thread();
}

// -----------------------------------------------------------------------
// Test hazard pointer guard RAII.
// -----------------------------------------------------------------------
void test_hp_guard() {
    auto& mgr = lfqueue::HazardPointerManager::instance();
    lfqueue::HPThreadData* td = mgr.register_thread();

    int dummy = 0;
    {
        lfqueue::HazardPointerGuard<int> guard(td, 0, &dummy);
        void* hp_val = td->hps[0].ptr.load(std::memory_order_acquire);
        TEST("guard_sets_hp", hp_val == &dummy);
    }
    // After guard destruction, the HP should be cleared.
    void* hp_val = td->hps[0].ptr.load(std::memory_order_acquire);
    TEST("guard_clears_hp", hp_val == nullptr);

    mgr.unregister_thread();
}

// -----------------------------------------------------------------------
// Test retire and scan (single thread).
// -----------------------------------------------------------------------
void test_retire_single() {
    auto& mgr = lfqueue::HazardPointerManager::instance();
    lfqueue::HPThreadData* td = mgr.register_thread();

    // Allocate some memory and retire it.
    int* p1 = static_cast<int*>(std::malloc(sizeof(int)));
    int* p2 = static_cast<int*>(std::malloc(sizeof(int)));
    *p1 = 10;
    *p2 = 20;

    TEST("retire_before", td->retired_list.empty());
    mgr.retire(p1);
    TEST("retire_after_one", td->retired_count == 1);
    mgr.retire(p2);
    TEST("retire_after_two", td->retired_count == 2);

    // Manually trigger scan (should free both since no HP protects them).
    mgr.scan();
    TEST("scan_cleared", td->retired_count == 0);
    TEST("scan_list_empty", td->retired_list.empty());

    mgr.unregister_thread();
}

// -----------------------------------------------------------------------
// Test that a retired pointer protected by an HP is NOT freed.
// -----------------------------------------------------------------------
void test_retire_protected() {
    auto& mgr = lfqueue::HazardPointerManager::instance();
    lfqueue::HPThreadData* td = mgr.register_thread();

    int* protected_ptr = static_cast<int*>(std::malloc(sizeof(int)));
    *protected_ptr = 42;

    // Set hazard pointer to protect this pointer.
    td->hps[0].ptr.store(protected_ptr, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    mgr.retire(protected_ptr);
    // After retire + scan, the pointer should still be in the retired list
    // because it's protected.
    TEST("protected_ptr_remains", td->retired_count == 1);

    // Clear the HP and scan again.
    td->hps[0].ptr.store(nullptr, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    mgr.scan();
    TEST("protected_ptr_freed_after_clear", td->retired_count == 0);

    mgr.unregister_thread();
}

// -----------------------------------------------------------------------
// Multi-threaded HP test.
// -----------------------------------------------------------------------
void test_mt_hp() {
    constexpr int kNumThreads = 4;
    std::atomic<int> ready{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([&]() {
            auto& mgr = lfqueue::HazardPointerManager::instance();
            lfqueue::HPThreadData* td = mgr.register_thread();

            ready.fetch_add(1, std::memory_order_release);

            // Spin until all threads are registered.
            while (ready.load(std::memory_order_acquire) < kNumThreads) {
                std::this_thread::yield();
            }

            // Allocate and retire a pointer.
            int* p = static_cast<int*>(std::malloc(sizeof(int)));
            *p = 123;
            mgr.retire(p);

            mgr.unregister_thread();
        });
    }

    for (auto& t : threads) t.join();

    // No crash = success.
    TEST("mt_hp_no_crash", true);
}

// -----------------------------------------------------------------------
int main() {
    fprintf(stdout, "=== lockfree-queue hazard pointer tests ===\n\n");

    test_register_unregister();
    test_hp_guard();
    test_retire_single();
    test_retire_protected();
    test_mt_hp();

    fprintf(stdout, "\n=== Results: %d passed, %d failed ===\n",
            tests_passed, tests_failed);

    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
