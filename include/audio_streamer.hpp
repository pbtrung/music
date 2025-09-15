#pragma once

#include <filesystem>
#include <thread>

#include "atomic_queues.hpp"
#include "json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

class AudioStreamManager {
  private:
    jdz::SpscQueue<json> queue;
    json config;
    std::jthread producer_thread;
    std::jthread consumer_thread;

    // Producer methods
    json get_track();
    std::string download_track(const json &track);
    void push_track(json track);
    void producer_loop();

    // Consumer methods
    json pop_track();
    void print_info(const json &track);
    void consumer_loop();

    // Utility methods
    void cleanup_file(const fs::path &path);
    void cleanup_cid_files(const json &track);

    int compute_track_gain(const json &config, const json &track);

  public:
    explicit AudioStreamManager(const json &cfg, int queue_size = 4);
    ~AudioStreamManager();

    // Disable copy constructor and assignment operator
    AudioStreamManager(const AudioStreamManager &) = delete;
    AudioStreamManager &operator=(const AudioStreamManager &) = delete;

    // Enable move constructor and assignment operator
    AudioStreamManager(AudioStreamManager &&) = default;
    AudioStreamManager &operator=(AudioStreamManager &&) = default;

    void start();
    void stop();
    void wait();

    // Get current queue size for monitoring
    size_t get_queue_size() const;
};