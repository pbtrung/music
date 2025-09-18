#include <iostream>

#include <fmt/base.h>
#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include "audio_processor.hpp"
#include "cppcodec/base64_url_unpadded.hpp"
#include "downloader.hpp"
#include "file_block_reader.hpp"
#include "migrator.hpp"
#include "nntp_client.hpp"
#include "rapidyenc.hpp"
#include "thread_pool.hpp"
#include "track.hpp"
#include "utils.hpp"
#include "wirehair/wirehair.h"

using base64_url_unpadded = cppcodec::base64_url_unpadded;

Migrator::Migrator(const json &cfg, int queue_size)
    : queue(queue_size), config(cfg) {
    SPDLOG_TRACE("Migrator constructed with queue size: {}", queue_size);
}

Migrator::~Migrator() {
    stop();
}

void Migrator::start() {
    SPDLOG_TRACE("Starting Migrator");

    // Clean up output directory
    fs::remove_all(config["output"].get<std::string>());

    // Start producer and consumer threads
    producer_thread = std::jthread([this]() { producer_loop(); });
    consumer_thread = std::jthread([this]() { consumer_loop(); });

    SPDLOG_TRACE("Migrator started");
}

void Migrator::stop() {
    SPDLOG_TRACE("Stopping Migrator");

    if (producer_thread.joinable()) {
        producer_thread.request_stop();
    }
    if (consumer_thread.joinable()) {
        consumer_thread.request_stop();
    }

    SPDLOG_TRACE("Migrator stopped");
}

void Migrator::wait() {
    if (producer_thread.joinable()) {
        producer_thread.join();
    }
    if (consumer_thread.joinable()) {
        consumer_thread.join();
    }
}

size_t Migrator::get_queue_size() const {
    return queue.size();
}

json Migrator::get_track(size_t i) {
    std::string query =
        fmt::format("SELECT * FROM tracks WHERE track_id = {}", i);
    SPDLOG_TRACE("{}", query);

    const std::string url =
        fmt::format("https://{}.r2.cloudflarestorage.com/{}/{}",
                    config["r2"]["account_id"].get<std::string>(),
                    config["r2"]["bucket"].get<std::string>(),
                    config["r2"]["db_file"].get<std::string>());
    Track track = Track::load(url, config, query);
    return track.get_json();
}

std::string Migrator::download_track(const json &track) {
    Downloader dl(config, track);
    dl.download_file();

    if (!dl.succeeded()) {
        return "";
    }

    return dl.assemble_file().value();
}

void Migrator::push_track(json track) {
    while (!queue.try_push(std::move(track))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

int Migrator::compute_track_gain(const json &config, const json &track) {
    double gain_db = -7.0;
    try {
        const fs::path output_dir = config["output"].get<std::string>();
        const std::string filename = track["filename"].get<std::string>();
        const fs::path file_path = output_dir / filename;

        gain_db = TrackGainAnalyzer::compute_and_write_track_gain(file_path);
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Error: {}", e.what());
        std::exit(EXIT_FAILURE);
    } catch (...) {
        SPDLOG_TRACE("Unknown error");
        std::exit(EXIT_FAILURE);
    }

    double gain_multiplier = std::pow(10.0, gain_db / 20.0);
    int gain_fixed = static_cast<int>(gain_multiplier * 32768.0);
    return gain_fixed;
}

std::string Migrator::generate_hmac_key() {
    std::vector<std::byte> hmac_key(32);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(hmac_key.data()),
                   hmac_key.size()) != 1) {
        SPDLOG_TRACE("Error: RAND_bytes failed");
        throw std::runtime_error("Failed to generate HMAC key");
    }
    return base64_url_unpadded::encode(hmac_key);
}

json Migrator::prepare_track_for_processing(size_t track_id) {
    json track = get_track(track_id);

    std::string filename = download_track(track);
    if (filename.empty()) {
        throw std::runtime_error("Download failed for track " +
                                 std::to_string(track_id));
    }

    track["filename"] = filename;
    track["max_value"] = config["max_value"].get<int>();
    track["gain_fixed"] = compute_track_gain(config, track);
    track["hmac_key"] = generate_hmac_key();

    return track;
}

void Migrator::producer_loop() {
    SPDLOG_TRACE("Producer start");

    const int start = config["migrate"]["start"].get<int>();
    const int end = config["migrate"]["end"].get<int>();

    for (int i = start; i < end; i++) {
        SPDLOG_TRACE("Producer processing track {}", i);

        json track;
        try {
            track = prepare_track_for_processing(i);
            const std::string &filename = track["filename"].get<std::string>();

            SPDLOG_TRACE("Push: {}", filename);
            push_track(std::move(track));

        } catch (const std::exception &e) {
            SPDLOG_TRACE("Producer error for track {}: {}", i, e.what());
            spdlog::dump_backtrace();
            cleanup_cid_files(track);
            std::exit(EXIT_FAILURE);
        } catch (...) {
            SPDLOG_TRACE("Producer unknown error for track {}", i);
            spdlog::dump_backtrace();
            cleanup_cid_files(track);
            std::exit(EXIT_FAILURE);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Signal end of processing
    push_track(json(nullptr));
}

json Migrator::pop_track() {
    json track;
    while (!queue.try_pop(track)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return track;
}

void Migrator::print_info(const json &track) {
    if (track.empty()) {
        SPDLOG_TRACE("Empty track");
        return;
    }

    const std::string filename = track.value("filename", "UNKNOWN");
    const std::string album = track["album"]["path"].get<std::string>();
    const std::string name = track.value("track_name", "UNKNOWN");
    const int id = track.value("track_id", 0);
    const int total = track.value("max_value", 0);
    const int cids = track.contains("cids") ? track["cids"].size() : 0;

    fmt::print("PROCESSING: {}\n", filename);
    fmt::print("  track: {} / {}\n", Utilities::format_commas(id),
               Utilities::format_commas(total));
    fmt::print("  album: {}\n", album);
    fmt::print("  filename: {}\n", name);

    if (cids == 1) {
        const std::string cid = track.contains("cids") && !track["cids"].empty()
                                    ? track["cids"][0].get<std::string>()
                                    : "UNKNOWN";
        fmt::print("  info: {} -> {}\n", cid, filename);
    } else {
        fmt::print("  info: {} CIDs -> {}\n", cids, filename);
    }
    std::cout.flush();
}

EncodedPiece Migrator::process_piece(size_t block_index, int piece_id,
                                     const std::vector<std::byte> &piece_data,
                                     const std::string &hmac_key) {
    std::string hmac = Utilities::hmac_sha3_256(hmac_key, piece_data);
    if (hmac.empty()) {
        throw std::runtime_error("Failed to compute hmac_sha3_256");
    }

    RapidYenc yenc;
    std::string encoded_data = yenc.encode_to_string(piece_data);

    return {block_index, piece_id, std::move(hmac), std::move(encoded_data)};
}

std::unique_ptr<WirehairCodec_t, decltype(&wirehair_free)>
Migrator::create_wirehair_encoder(const std::vector<std::byte> &data) {
    if (wirehair_init() != Wirehair_Success) {
        throw std::runtime_error("Failed to initialize Wirehair library");
    }

    const auto &usenet_config = config["usenet"];
    uint32_t orig_count = usenet_config["wh_k"].get<uint32_t>();
    uint32_t block_size = (data.size() + orig_count - 1) / orig_count;

    WirehairCodec encoder =
        wirehair_encoder_create(nullptr, data.data(), data.size(), block_size);

    if (!encoder) {
        throw std::runtime_error("Failed to create Wirehair encoder");
    }

    return std::unique_ptr<WirehairCodec_t, decltype(&wirehair_free)>(
        encoder, wirehair_free);
}

void Migrator::process_block_with_wirehair(size_t block_index,
                                           const std::vector<std::byte> &data,
                                           const json &track) {
    auto encoder = create_wirehair_encoder(data);

    const auto &usenet_config = config["usenet"];
    uint32_t orig_count = usenet_config["wh_k"].get<uint32_t>();
    uint32_t redu_count = usenet_config["wh_n"].get<uint32_t>();
    uint32_t block_size = (data.size() + orig_count - 1) / orig_count;

    const int thread_count =
        config["ncores"].get<int>() * config["mul_factor"].get<int>();
    dp::ThreadPool thread_pool(thread_count);

    const std::string hmac_key = track["hmac_key"].get<std::string>();

    for (int piece_id = 0; piece_id < static_cast<int>(redu_count);
         ++piece_id) {
        std::vector<std::byte> piece_data(block_size);
        uint32_t bytes_out = block_size;

        WirehairResult result = wirehair_encode(
            encoder.get(), piece_id, piece_data.data(), block_size, &bytes_out);

        if (result != Wirehair_Success) {
            throw std::runtime_error(
                fmt::format("Failed to encode piece {}: error {}", piece_id,
                            static_cast<int>(result)));
        }

        // Resize to actual data size
        piece_data.resize(bytes_out);

        // Submit work to thread pool
        thread_pool.enqueue_detach([this, block_index, piece_id,
                                    piece_data = std::move(piece_data),
                                    hmac_key]() {
            try {
                EncodedPiece result =
                    process_piece(block_index, piece_id, piece_data, hmac_key);

                // Submit it to NNTP
                // handle_encoded_piece(std::move(result));

            } catch (const std::exception &e) {
                SPDLOG_ERROR("Error processing piece {}-{}: {}", block_index,
                             piece_id, e.what());
            }
        });
    }

    SPDLOG_TRACE("All download tasks enqueued, waiting for completion");
    thread_pool.wait_for_tasks();
    SPDLOG_TRACE("All download tasks completed");
}

void Migrator::consumer_loop() {
    SPDLOG_TRACE("Consumer start");

    while (true) {
        SPDLOG_TRACE("Consumer loop iteration");
        fs::path file_path;

        try {
            json track = pop_track();

            // Check for termination signal
            if (!track.contains("track_name")) {
                SPDLOG_TRACE("Received termination signal");
                break;
            }

            const fs::path output_dir = config["output"].get<std::string>();
            const std::string filename = track["filename"].get<std::string>();
            file_path = output_dir / filename;

            SPDLOG_TRACE("Processing: {}", filename);
            print_info(track);

            FileBlockReader reader(file_path);
            reader.process_blocks(
                [this, track](size_t block_index,
                              const std::vector<std::byte> &data) {
                    process_block_with_wirehair(block_index, data, track);
                });

        } catch (const std::exception &e) {
            SPDLOG_ERROR("Consumer error: {}", e.what());
            spdlog::dump_backtrace();
            cleanup_file(file_path);
            std::exit(EXIT_FAILURE);
        } catch (...) {
            SPDLOG_ERROR("Consumer unknown error");
            spdlog::dump_backtrace();
            cleanup_file(file_path);
            std::exit(EXIT_FAILURE);
        }
    }

    SPDLOG_TRACE("Consumer finished");
}

void Migrator::cleanup_file(const fs::path &path) {
    if (fs::exists(path)) {
        try {
            fs::remove(path);
            SPDLOG_TRACE("Removed: {}", path.string());
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Remove failed {}: {}", path.string(), e.what());
            spdlog::dump_backtrace();
        }
    }
}

void Migrator::cleanup_cid_files(const json &track) {
    if (!track.contains("cids")) {
        return;
    }

    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const fs::path output_dir = config["output"].get<std::string>();

    for (const auto &cid : cids) {
        const fs::path path = output_dir / cid;
        cleanup_file(path);
    }
}