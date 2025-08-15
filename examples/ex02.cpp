#include <chrono>
#include <iostream>
#include <thread>

#include "thread_pool.hpp"

void run_task(int i) {
    std::cout << "Task " << i << " started on thread "
              << std::this_thread::get_id() << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cout << "Task " << i << " finished\n";
}

int main() {
    // 3 worker threads, queue capacity 5
    ThreadPool pool(3, 5);

    // Submit some tasks
    for (int i = 0; i < 10; ++i) {
        pool.enqueue(run_task, i);
    }

    pool.shutdown();
    std::cout << "All tasks done, pool shut down\n";
    return 0;
}
