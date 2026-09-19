// Parallel loops for the compositor. A persistent pool of workers takes row (or column)
// chunks from a shared counter, so a pass costs no thread creation and bands that finish
// early pick up more work. The calling thread joins in; a loop started from inside a
// worker runs serially.
#pragma once
#include <functional>

namespace compositor {

/// Threads the pool uses (the machine's hardware concurrency, capped).
int workerCount();

/// Runs `body(y0, y1)` over [begin, end) in chunks of at least `minRows` rows; returns when every chunk is done.
void parallelFor(int begin, int end, int minRows, const std::function<void(int, int)>& body);

/// The same for any callable; `minRowsPerThread` also keeps small ranges serial.
template <typename Body>
void parallelRows(int begin, int end, Body body, int minRowsPerThread = 24) {
    parallelFor(begin, end, minRowsPerThread, [&](int y0, int y1) { body(y0, y1); });
}

} // namespace compositor
