#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

template <typename T> class BoundedQueue {
  public:
    explicit BoundedQueue(size_t capacity);

    bool push(const T &item);
    bool push(T &&item);

    std::optional<T> pop();

    void stop();

    bool empty() const;
    bool stopped() const;

  private:
    size_t capacity;
    mutable std::mutex mutex;
    std::condition_variable cond_empty;
    std::condition_variable cond_full;
    std::queue<T> queue;
    bool is_stopped;
};
