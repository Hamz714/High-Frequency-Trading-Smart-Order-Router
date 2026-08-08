#pragma once

#include <cstdint>
#include <cstddef>

// power of 2
constexpr int64_t LADDER_DEPTH = 256;
constexpr int64_t MASK_MODULO = LADDER_DEPTH - 1;
constexpr int64_t LADDER_WORDS = LADDER_DEPTH / 64;
constexpr int64_t SNAPSHOT_LEVELS = 5;
constexpr size_t DEFAULT_ORDER_POOL_CAPACITY = 4096;
// rounded up to a power of 2 by the queue constructors
constexpr size_t DEFAULT_QUEUE_CAPACITY = 4096;
constexpr int BATCH_LIMIT = 256;