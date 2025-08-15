#include <chrono>
#include <iostream>
#include <thread>

#include "bounded_queue.hpp"

void producer(BoundedQueue<int> &queue) {
    for (int i = 0; i < 6; ++i) {
        std::cout << "Producing " << i << "\n";
        queue.push(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    queue.stop();
}

void consumer(BoundedQueue<int> &queue) {
    while (true) {
        auto val_opt = queue.pop();
        if (!val_opt.has_value()) {
            std::cout << "Consumer stopped\n";
            break;
        }
        std::cout << "Consuming " << *val_opt << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

int main() {
    BoundedQueue<int> bq(3);

    std::jthread prod(producer, std::ref(bq));
    std::jthread cons1(consumer, std::ref(bq));
    std::jthread cons2(consumer, std::ref(bq));
}
