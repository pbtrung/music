#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <thread>
#include <vector>

#include "bounded_queue.hpp"

class ThreadPool {
  public:
    ThreadPool(size_t thread_count, size_t queue_capacity);
    ~ThreadPool();

    template <typename Func, typename... Args>
    auto enqueue(Func &&func, Args &&...args)
        -> std::future<std::invoke_result_t<Func, Args...>> {
        using return_t = std::invoke_result_t<Func, Args...>;

        auto task_ptr = std::make_shared<std::packaged_task<return_t()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));

        task_queue.push([task_ptr]() { (*task_ptr)(); });

        return task_ptr->get_future();
    }

    void shutdown(); // Wait for all tasks, then stop workers

  private:
    void worker();

    BoundedQueue<std::function<void()>> task_queue;
    std::vector<std::jthread> workers;

    std::atomic<size_t> active_tasks{0};
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
};
