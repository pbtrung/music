#pragma once

#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "atomic_queues.hpp"
#include "json.hpp"
#include "nntp_client.hpp"
#include "wirehair/wirehair.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// Forward declaration for encoded piece data
struct EncodedPiece {
    size_t block_index;
    int piece_id;
    std::string hmac;
    std::string encoded_data;
};

class Migrator {
  private:
    jdz::SpscQueue<json> queue;
    json config;
    std::jthread producer_thread;
    std::jthread consumer_thread;

    std::vector<std::vector<std::string>> res;

    // Producer methods
    json get_track(size_t i);
    std::string download_track(const json &track);
    void push_track(json track);
    void producer_loop();

    // New producer helper methods
    std::string generate_hmac_key();
    json prepare_track_for_processing(size_t track_id);

    // Consumer methods
    json pop_track();
    void print_info(const json &track);
    void consumer_loop();

    // New consumer helper methods
    EncodedPiece process_piece(size_t block_index, int piece_id,
                               const std::vector<std::byte> &piece_data,
                               const std::string &hmac_key);

    std::unique_ptr<WirehairCodec_t, decltype(&wirehair_free)>
    create_wirehair_encoder(const std::vector<std::byte> &data);

    void process_block_with_wirehair(size_t block_index,
                                     const std::vector<std::byte> &data,
                                     const json &track);

    // Utility methods
    void cleanup_file(const fs::path &path);
    void cleanup_cid_files(const json &track);
    int compute_track_gain(const json &config, const json &track);
    void validate_res() const;
    void post_with_retry(const NntpConnection &conn, const NntpMessage &msg,
                         size_t block_index, int piece_id, int max_retries);

  public:
    explicit Migrator(const json &cfg, int queue_size = 4);
    ~Migrator();

    // Disable copy constructor and assignment operator
    Migrator(const Migrator &) = delete;
    Migrator &operator=(const Migrator &) = delete;

    // Enable move constructor and assignment operator
    Migrator(Migrator &&) = default;
    Migrator &operator=(Migrator &&) = default;

    void start();
    void stop();
    void wait();

    // Get current queue size for monitoring
    size_t get_queue_size() const;
};