// Global heap-allocation counter (operator new is replaced in alloc_counter.cpp). Linked into the
// replay tool and the allocation test to prove the steady-state hot path never allocates.
#pragma once
#include <cstdint>

namespace itchbook {
std::uint64_t alloc_count() noexcept;
}
