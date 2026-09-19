// Row-parallel loops for the compositor: output rows are independent, so
// every per-pixel pass splits into bands across the machine's cores.
#pragma once
#include <algorithm>
#include <thread>
#include <vector>

namespace compositor {

inline int workerCount() {
    static int n = std::max(1u, std::min(64u, std::thread::hardware_concurrency()));
    return n;
}

/// Runs `body(y0, y1)` over [begin, end) in bands on all cores; serial for small ranges.
template <typename Body>
void parallelRows(int begin, int end, Body body, int minRowsPerThread = 24) {
    int rows = end - begin;
    if (rows <= 0) return;
    int threads = std::min(workerCount(), std::max(1, rows / minRowsPerThread));
    if (threads <= 1) { body(begin, end); return; }
    std::vector<std::thread> pool;
    pool.reserve(size_t(threads));
    int band = (rows + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        int y0 = begin + t * band, y1 = std::min(end, y0 + band);
        if (y0 >= y1) break;
        pool.emplace_back([=] { body(y0, y1); });
    }
    for (auto& th : pool) th.join();
}

} // namespace compositor
