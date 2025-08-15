#include "thread_pool.hpp"

ThreadPool::ThreadPool(size_t thread_count, size_t queue_capacity)
    : task_queue(queue_capacity) {
    for (size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back(&ThreadPool::worker, this);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    // Wait for all tasks to finish
    {
        std::unique_lock lock(wait_mutex);
        wait_cv.wait(lock, [this] {
            return active_tasks.load() == 0 && task_queue.empty();
        });
    }

    // Now stop the queue and let workers exit
    task_queue.stop();
    for (auto &t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void ThreadPool::worker() {
    while (true) {
        auto opt_task = task_queue.pop();
        if (!opt_task.has_value())
            break; // queue stopped

        active_tasks.fetch_add(1, std::memory_order_relaxed);

        opt_task.value()();

        active_tasks.fetch_sub(1, std::memory_order_relaxed);

        // Notify shutdown waiter if no more active tasks
        if (active_tasks.load() == 0 && task_queue.empty()) {
            std::lock_guard lock(wait_mutex);
            wait_cv.notify_all();
        }
    }
}
