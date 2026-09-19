#include "compositor/parallel.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace compositor {

namespace {

thread_local bool insideParallel = false;

/// One loop in flight: chunks are handed out through `next`; `busy` counts workers inside work().
struct Job {
    const std::function<void(int, int)>* body;
    int begin, end, chunk, chunks;
    std::atomic<int> next{0};
    int busy = 0;

    void work() {
        while (true) {
            int index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= chunks) return;
            int y0 = begin + index * chunk, y1 = std::min(end, y0 + chunk);
            (*body)(y0, y1);
        }
    }
};

class Pool {
public:
    static Pool& shared() { static Pool pool; return pool; }

    void run(Job& job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_ = &job;
            generation_++;
        }
        wake_.notify_all();
        job.work();
        // No worker can pick the job up once it is no longer current; wait for the ones that did to leave it.
        std::unique_lock<std::mutex> lock(mutex_);
        current_ = nullptr;
        finished_.wait(lock, [&] { return job.busy == 0; });
    }

private:
    Pool() {
        int n = workerCount() - 1;
        for (int i = 0; i < n; i++) workers_.emplace_back([this] { serve(); });
    }
    ~Pool() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        wake_.notify_all();
        for (auto& w : workers_) w.join();
    }

    void serve() {
        insideParallel = true;
        unsigned long long seen = 0;
        while (true) {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            Job* job = current_;
            if (!job) continue;
            job->busy++;
            lock.unlock();
            job->work();
            lock.lock();
            if (--job->busy == 0) finished_.notify_all();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_, finished_;
    Job* current_ = nullptr;
    unsigned long long generation_ = 0;
    bool stop_ = false;
};

} // namespace

int workerCount() {
    static int n = std::max(1u, std::min(64u, std::thread::hardware_concurrency()));
    return n;
}

void parallelFor(int begin, int end, int minRows, const std::function<void(int, int)>& body) {
    int rows = end - begin;
    if (rows <= 0) return;
    int threads = workerCount();
    if (threads <= 1 || insideParallel || rows < 2 * std::max(1, minRows)) { body(begin, end); return; }
    // Chunks about four per thread, never smaller than the caller's minimum, so late bands balance out.
    int chunk = std::max(std::max(1, minRows), (rows + threads * 4 - 1) / (threads * 4));
    Job job;
    job.body = &body;
    job.begin = begin;
    job.end = end;
    job.chunk = chunk;
    job.chunks = (rows + chunk - 1) / chunk;
    if (job.chunks <= 1) { body(begin, end); return; }
    insideParallel = true;
    Pool::shared().run(job);
    insideParallel = false;
}

} // namespace compositor
