#include <functional>

#include "bounded_queue.hpp"

template <typename T>
BoundedQueue<T>::BoundedQueue(size_t capacity)
    : capacity(capacity), is_stopped(false) {}

template <typename T> bool BoundedQueue<T>::push(const T &item) {
    std::unique_lock lock(mutex);
    cond_full.wait(lock,
                   [this] { return queue.size() < capacity || is_stopped; });
    if (is_stopped)
        return false;
    queue.push(item);
    cond_empty.notify_one();
    return true;
}

template <typename T> bool BoundedQueue<T>::push(T &&item) {
    std::unique_lock lock(mutex);
    cond_full.wait(lock,
                   [this] { return queue.size() < capacity || is_stopped; });
    if (is_stopped)
        return false;
    queue.push(std::move(item));
    cond_empty.notify_one();
    return true;
}

template <typename T> std::optional<T> BoundedQueue<T>::pop() {
    std::unique_lock lock(mutex);
    cond_empty.wait(lock, [this] { return !queue.empty() || is_stopped; });
    if (queue.empty())
        return std::nullopt;
    T value = std::move(queue.front());
    queue.pop();
    cond_full.notify_one();
    return value;
}

template <typename T> void BoundedQueue<T>::stop() {
    {
        std::scoped_lock lock(mutex);
        is_stopped = true;
    }
    cond_empty.notify_all();
    cond_full.notify_all();
}

template <typename T> bool BoundedQueue<T>::empty() const {
    std::scoped_lock lock(mutex);
    return queue.empty();
}

template <typename T> bool BoundedQueue<T>::stopped() const {
    std::scoped_lock lock(mutex);
    return is_stopped;
}

// Explicit instantiations if needed
template class BoundedQueue<int>;
template class BoundedQueue<std::function<void()>>;
