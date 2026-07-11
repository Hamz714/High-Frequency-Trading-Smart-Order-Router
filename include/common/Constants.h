#pragma once

#include <cstdint>
#include <cstddef>

// power of 2
constexpr int64_t LADDER_DEPTH = 256;
constexpr int64_t MASK_MODULO = LADDER_DEPTH - 1;
constexpr int64_t SNAPSHOT_LEVELS = 5;
// power of 2
constexpr size_t QUEUE_SIZE = 4096;
constexpr int BATCH_LIMIT = 256;