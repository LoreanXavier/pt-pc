#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

namespace pt {

// Runs fn(chunk, begin, end) over [0, count) split into at most `max_chunks` contiguous chunks in order, chunk 0 on the calling
// thread. The split depends only on count and the chunk count, so work that writes each chunk's own output is deterministic.
template <typename Fn>
void ParallelChunks(size_t count, size_t chunks, Fn&& fn) {
    chunks = std::max<size_t>(1, std::min(chunks, count));
    if (chunks <= 1) {
        fn(size_t{0}, size_t{0}, count);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(chunks - 1);
    for (size_t c = 1; c < chunks; ++c) {
        threads.emplace_back([&fn, c, count, chunks] { fn(c, count * c / chunks, count * (c + 1) / chunks); });
    }
    fn(size_t{0}, size_t{0}, count / chunks);
    for (std::thread& t : threads) {
        t.join();
    }
}

// the chunk count for work of `count` items: one per core up to 8, none below `min_per_chunk` items each
inline size_t ParallelChunkCount(size_t count, size_t min_per_chunk) {
    const size_t cores = std::max<size_t>(1, std::thread::hardware_concurrency());
    return std::max<size_t>(1, std::min<size_t>({cores, size_t{8}, count / std::max<size_t>(1, min_per_chunk)}));
}

}
