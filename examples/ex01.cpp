#include <chrono>
#include <iostream>
#include <thread>

#include "atomic_queues.hpp"

void producer(jdz::SpscQueue<int> &queue) {
    for (int i = 0; i < 6; ++i) {
        std::cout << "Producing " << i << "\n";
        queue.push(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // push sentinel values to signal consumers to stop
    queue.push(-1);
    queue.push(-1);
}

void consumer(jdz::SpscQueue<int> &queue, int id) {
    int value;
    while (true) {
        if (queue.try_pop(value)) {
            if (value == -1) {
                std::cout << "Consumer " << id << " stopped\n";
                break;
            }
            std::cout << "Consumer " << id << " consumed " << value << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        } else {
            // avoid busy spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

int main() {
    // SPSC queue: capacity must be > 1
    jdz::SpscQueue<int> queue(8);

    std::jthread prod(producer, std::ref(queue));
    std::jthread cons1(consumer, std::ref(queue), 1);
    std::jthread cons2(consumer, std::ref(queue), 2);

    prod.join();
    cons1.join();
    cons2.join();
}
