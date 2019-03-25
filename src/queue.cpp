#include "lfqueue/queue.h"

// -----------------------------------------------------------------------
// This file exists to ensure the template instantiations that users
// are most likely to need are compiled once (reducing compile time
// for heavy users).  Explicit template instantiations.
//
// Note: the full implementation lives in the header because it's
// a template.  This .cpp just forces instantiation for common types.
// -----------------------------------------------------------------------

namespace lfqueue {

// Explicit instantiations for common queue element types.
template class MPMCQueue<int>;
template class MPMCQueue<std::int64_t>;
template class MPMCQueue<void*>;

// Conditional instantiation for trivially-copyable 32-byte structs.
struct alignas(32) MarketTick {
    std::int64_t timestamp;
    std::int64_t price;
    std::int64_t volume;
    std::int64_t flags;
};
static_assert(sizeof(MarketTick) == 32, "MarketTick must be 32 bytes");
template class MPMCQueue<MarketTick>;

}  // namespace lfqueue
