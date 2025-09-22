#include <fmt/base.h>
#include <iostream>
#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include "audio_processor.hpp"
#include "cppcodec/base64_url_unpadded.hpp"
#include "downloader.hpp"
#include "file_block_reader.hpp"
#include "migrator.hpp"
#include "rapidyenc.hpp"
#include "sqlitedb.hpp"
#include "thread_pool.hpp"
#include "track.hpp"
#include "utils.hpp"
#include "wirehair/wirehair.h"

using base64_url_unpadded = cppcodec::base64_url_unpadded;

namespace {
constexpr int DEFAULT_QUEUE_SIZE = 4;
constexpr int DEFAULT_MAX_RETRIES = 10;
constexpr int RETRY_DELAY_MS = 500;
constexpr int PRODUCER_DELAY_MS = 100;
constexpr int QUEUE_WAIT_MS = 1000;
constexpr double DEFAULT_GAIN_DB = -7.0;
constexpr double GAIN_MULTIPLIER_SCALE = 32768.0;
constexpr size_t HMAC_KEY_SIZE = 32;
constexpr int RANDOM_STRING_LENGTH = 45;
constexpr int MSG_ID_EXT_LENGTH = 10;
constexpr int FROM_USER_LENGTH = 10;
constexpr int FROM_DOMAIN_LENGTH = 10;
constexpr int FROM_TLD_LENGTH = 5;
} // namespace

// Configuration wrapper for type safety
class MigratorConfig {
  public:
    explicit MigratorConfig(const json &config) : config(config) {
        validate_config();
    }

    std::string output_dir() const {
        return config["output"].get<std::string>();
    }
    int migrate_start() const {
        return config["migrate"]["start"].get<int>();
    }
    int migrate_end() const {
        return config["migrate"]["end"].get<int>();
    }
    int max_value() const {
        return config["max_value"].get<int>();
    }

    // R2 configuration
    std::string r2_account_id() const {
        return config["r2"]["account_id"].get<std::string>();
    }
    std::string r2_bucket() const {
        return config["r2"]["bucket"].get<std::string>();
    }
    std::string r2_db_file() const {
        return config["r2"]["db_file"].get<std::string>();
    }

    // Usenet configuration
    std::string usenet_hostname() const {
        return config["usenet"]["hostname"].get<std::string>();
    }
    int usenet_port() const {
        return config["usenet"]["port"].get<int>();
    }
    std::string usenet_username() const {
        return config["usenet"]["username"].get<std::string>();
    }
    std::string usenet_password() const {
        return config["usenet"]["password"].get<std::string>();
    }
    std::string usenet_newsgroups() const {
        return config["usenet"]["newsgroups"].get<std::string>();
    }
    std::string usenet_db() const {
        return config["usenet"]["db"].get<std::string>();
    }
    int usenet_num_conns() const {
        return config["usenet"]["num_conns"].get<int>();
    }

    // Wirehair configuration
    uint32_t wh_k() const {
        return config["usenet"]["wh_k"].get<uint32_t>();
    }
    uint32_t wh_n() const {
        return config["usenet"]["wh_n"].get<uint32_t>();
    }

    const json &raw() const {
        return config;
    }

  private:
    const json &config;

    void validate_config() const {
        // Validate required configuration keys
        const std::vector<std::string> required_keys = {
            "output", "migrate", "max_value", "r2", "usenet"};

        for (const auto &key : required_keys) {
            if (!config.contains(key)) {
                throw std::runtime_error(
                    fmt::format("Missing required config key: {}", key));
            }
        }

        // Validate migrate range
        if (migrate_start() > migrate_end()) {
            throw std::runtime_error(
                "migrate.start cannot be greater than migrate.end");
        }

        // Validate Wirehair parameters
        if (wh_k() == 0 || wh_n() == 0) {
            throw std::runtime_error(
                "Wirehair parameters wh_k and wh_n must be greater than 0");
        }

        if (wh_n() < wh_k()) {
            throw std::runtime_error("Wirehair parameter wh_n must be >= wh_k");
        }
    }
};

// Implementation
Migrator::Migrator(const json &cfg, int queue_size)
    : queue(queue_size > 0 ? queue_size : DEFAULT_QUEUE_SIZE), config(cfg) {
    SPDLOG_TRACE("Migrator constructed with queue size: {}", queue_size);
}

Migrator::~Migrator() {
    stop();
}

void Migrator::start() {
    SPDLOG_TRACE("Starting Migrator");

    MigratorConfig migrator_config(config);

    // Clean up output directory
    fs::remove_all(migrator_config.output_dir());
    fs::create_directories(migrator_config.output_dir());

    // Start producer and consumer threads
    producer_thread = std::jthread([this]() { producer_loop(); });
    consumer_thread = std::jthread([this]() { consumer_loop(); });

    SPDLOG_TRACE("Migrator started successfully");
}

void Migrator::stop() {
    SPDLOG_TRACE("Stopping Migrator");

    if (producer_thread.joinable()) {
        producer_thread.request_stop();
        producer_thread.join();
    }
    if (consumer_thread.joinable()) {
        consumer_thread.request_stop();
        consumer_thread.join();
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

json Migrator::get_track(size_t track_id) {
    MigratorConfig migrator_config(config);

    const std::string query =
        fmt::format("SELECT * FROM tracks WHERE track_id = {}", track_id);
    SPDLOG_TRACE("Executing query: {}", query);

    const std::string url =
        fmt::format("https://{}.r2.cloudflarestorage.com/{}/{}",
                    migrator_config.r2_account_id(),
                    migrator_config.r2_bucket(), migrator_config.r2_db_file());

    Track track = Track::load(url, config, query);
    return track.get_json();
}

std::string Migrator::download_track(const json &track) {
    Downloader dl(config, track);
    dl.download_file();

    if (!dl.succeeded()) {
        return "";
    }

    auto result = dl.assemble_file();
    return result.has_value() ? result.value() : "";
}

void Migrator::push_track(json track) {
    while (!queue.try_push(std::move(track))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(QUEUE_WAIT_MS));
    }
}

json Migrator::pop_track() {
    json track;
    while (!queue.try_pop(track)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(QUEUE_WAIT_MS));
    }
    return track;
}

std::string Migrator::generate_hmac_key() {
    std::vector<std::byte> hmac_key(HMAC_KEY_SIZE);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(hmac_key.data()),
                   hmac_key.size()) != 1) {
        throw std::runtime_error(
            "Failed to generate HMAC key using RAND_bytes");
    }
    return base64_url_unpadded::encode(hmac_key);
}

double Migrator::compute_track_gain(const json &track) {
    try {
        MigratorConfig migrator_config(config);
        const fs::path output_dir = migrator_config.output_dir();
        const std::string filename = track["filename"].get<std::string>();
        const fs::path file_path = output_dir / filename;

        return TrackGainAnalyzer::compute_and_write_track_gain(file_path);
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Failed to compute track gain: {}, using default",
                     e.what());
        return DEFAULT_GAIN_DB;
    }
}

json Migrator::prepare_track_for_processing(size_t track_id) {
    json track = get_track(track_id);

    std::string filename = download_track(track);
    if (filename.empty()) {
        throw std::runtime_error(
            fmt::format("Download failed for track {}", track_id));
    }

    MigratorConfig migrator_config(config);

    track["filename"] = filename;
    track["max_value"] = migrator_config.max_value();
    track["hmac_key"] = generate_hmac_key();

    double gain_db = compute_track_gain(track);
    double gain_multiplier = std::pow(10.0, gain_db / 20.0);
    int gain_fixed = static_cast<int>(gain_multiplier * GAIN_MULTIPLIER_SCALE);

    track["gain_db"] = gain_db;
    track["gain_fixed"] = gain_fixed;

    return track;
}

void Migrator::producer_loop() {
    SPDLOG_TRACE("Producer thread started");

    MigratorConfig migrator_config(config);
    const int start = migrator_config.migrate_start();
    const int end = migrator_config.migrate_end();

    for (int i = start; i <= end; ++i) {
        SPDLOG_TRACE("Producer processing track {}", i);

        try {
            json track = prepare_track_for_processing(i);

            // Always cleanup files at scope exit
            ScopeGuard cleanup([&] {
                if (track.contains("cids")) {
                    cleanup_cid_files(track);
                }
            });

            const std::string &filename = track["filename"].get<std::string>();
            SPDLOG_TRACE("Queuing track: {}", filename);
            push_track(std::move(track));

            std::this_thread::sleep_for(
                std::chrono::milliseconds(PRODUCER_DELAY_MS));
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Producer error for track {}: {}", i, e.what());
            std::exit(EXIT_FAILURE);
        }
    }

    // Signal end of processing
    push_track(json(nullptr));
    SPDLOG_TRACE("Producer thread finished");
}

void Migrator::print_info(const json &track) {
    if (track.empty() || !track.contains("filename")) {
        SPDLOG_TRACE("Empty or invalid track");
        return;
    }

    const std::string filename = track.value("filename", "UNKNOWN");
    const std::string album =
        track.contains("album") && track["album"].contains("path")
            ? track["album"]["path"].get<std::string>()
            : "UNKNOWN";
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

    std::string encoded_data = RapidYenc::encode_to_string(piece_data);
    return {block_index, piece_id, std::move(hmac), std::move(encoded_data)};
}

std::unique_ptr<WirehairCodec_t, decltype(&wirehair_free)>
Migrator::create_wirehair_encoder(const std::vector<std::byte> &data) {
    if (wirehair_init() != Wirehair_Success) {
        throw std::runtime_error("Failed to initialize Wirehair library");
    }

    MigratorConfig migrator_config(config);
    uint32_t orig_count = migrator_config.wh_k();
    uint32_t block_size = (data.size() + orig_count - 1) / orig_count;

    WirehairCodec encoder =
        wirehair_encoder_create(nullptr, data.data(), data.size(), block_size);
    if (!encoder) {
        throw std::runtime_error("Failed to create Wirehair encoder");
    }

    return std::unique_ptr<WirehairCodec_t, decltype(&wirehair_free)>(
        encoder, wirehair_free);
}

void Migrator::post_with_retry(const NntpConnection &conn,
                               const NntpMessage &msg, size_t block_index,
                               int piece_id, int max_retries) {
    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        try {
            NntpClient::post_message(conn, msg);
            return;
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Post attempt {} failed for block {} piece {}: {}",
                         attempt, block_index, piece_id, e.what());
            if (attempt >= max_retries) {
                throw;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(RETRY_DELAY_MS));
        }
    }
}

void Migrator::process_block_with_wirehair(size_t block_index,
                                           const std::vector<std::byte> &data,
                                           const json &track) {
    auto encoder = create_wirehair_encoder(data);

    MigratorConfig migrator_config(config);
    uint32_t orig_count = migrator_config.wh_k();
    uint32_t redu_count = migrator_config.wh_n();
    uint32_t block_size = (data.size() + orig_count - 1) / orig_count;
    int thread_count = migrator_config.usenet_num_conns();

    dp::ThreadPool thread_pool(thread_count);
    const std::string hmac_key = track["hmac_key"].get<std::string>();

    // Ensure result containers are properly sized
    if (res.size() <= block_index) {
        res.resize(block_index + 1);
        sizes.resize(block_index + 1);
    }
    res[block_index].resize(redu_count);
    sizes[block_index].resize(redu_count);

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

        NntpConnection conn{.hostname = migrator_config.usenet_hostname(),
                            .port = migrator_config.usenet_port(),
                            .username = migrator_config.usenet_username(),
                            .password = migrator_config.usenet_password(),
                            .use_ssl = true};

        // Submit work to thread pool
        thread_pool.enqueue_detach(
            [this, block_index, piece_id, piece_data = std::move(piece_data),
             hmac_key, conn, &track, &migrator_config]() {
                try {
                    EncodedPiece encoded_piece = process_piece(
                        block_index, piece_id, piece_data, hmac_key);

                    NntpMessage msg;
                    msg.subject =
                        Utilities::generate_random_string(RANDOM_STRING_LENGTH);
                    msg.from = track["from"].get<std::string>();
                    msg.newsgroups = migrator_config.usenet_newsgroups();
                    msg.body = encoded_piece.encoded_data;
                    msg.message_id =
                        fmt::format("<{}@{}>", encoded_piece.hmac,
                                    track["msg_id_ext"].get<std::string>());

                    post_with_retry(conn, msg, block_index, piece_id,
                                    DEFAULT_MAX_RETRIES);

                    res[block_index][piece_id] = std::move(encoded_piece.hmac);
                    sizes[block_index][piece_id] = piece_data.size();

                    SPDLOG_TRACE(
                        "Processed: track_id={}, block_index={}, piece_id={}",
                        track["track_id"].get<int>(), block_index, piece_id);

                } catch (const std::exception &e) {
                    SPDLOG_TRACE("Error processing piece {}-{}: {}",
                                 block_index, piece_id, e.what());
                    res[block_index][piece_id] = "";
                    sizes[block_index][piece_id] = 0;
                }
            });
    }

    SPDLOG_TRACE(
        "All encoding tasks enqueued for block {}, waiting for completion",
        block_index);
    thread_pool.wait_for_tasks();
    SPDLOG_TRACE("All encoding tasks completed for block {}", block_index);
}

void Migrator::validate_res() const {
    for (size_t block_index = 0; block_index < res.size(); ++block_index) {
        for (size_t piece_id = 0; piece_id < res[block_index].size();
             ++piece_id) {
            if (res[block_index][piece_id].empty()) {
                throw std::runtime_error(
                    fmt::format("Missing CID for block {} piece {}",
                                block_index, piece_id));
            }
        }
    }
}

void Migrator::consumer_loop() {
    SPDLOG_TRACE("Consumer thread started");

    MigratorConfig migrator_config(config);
    db::SqliteDb database(migrator_config.usenet_db());
    database.execute("CREATE TABLE IF NOT EXISTS tracks "
                     "(track_id INTEGER PRIMARY KEY, track BLOB NOT NULL)");

    while (true) {
        SPDLOG_TRACE("Consumer loop iteration");

        try {
            if (!RapidYenc::initialize()) {
                throw std::runtime_error(
                    "Failed to initialize RapidYenc library");
            }

            json track = pop_track();

            auto start = std::chrono::steady_clock::now();

            // Check for termination signal
            if (!track.contains("track_name")) {
                SPDLOG_TRACE("Consumer received termination signal");
                break;
            }

            const fs::path output_dir = migrator_config.output_dir();
            const std::string filename = track["filename"].get<std::string>();
            fs::path file_path = output_dir / filename;

            // Always cleanup file at scope exit
            ScopeGuard cleanup([&] { cleanup_file(file_path); });

            SPDLOG_TRACE("Processing track: {}", filename);
            print_info(track);

            FileBlockReader reader(file_path);
            auto block_sizes = reader.calculate_block_sizes();

            // Initialize result containers
            res.clear();
            res.resize(block_sizes.size());
            sizes.clear();
            sizes.resize(block_sizes.size());

            // Generate unique identifiers for this track
            track["msg_id_ext"] =
                Utilities::generate_random_string(MSG_ID_EXT_LENGTH);
            track["from"] = fmt::format(
                "{}@{}.{}", Utilities::generate_random_string(FROM_USER_LENGTH),
                Utilities::generate_random_string(FROM_DOMAIN_LENGTH),
                Utilities::generate_random_string(FROM_TLD_LENGTH));

            // Process all blocks
            reader.process_blocks(
                [this, &track](size_t block_index,
                               const std::vector<std::byte> &data) {
                    process_block_with_wirehair(block_index, data, track);
                });

            validate_res();

            // Finalize track data
            track["cids"] = res;
            track["sizes"] = sizes;
            track["hmac_hash"] = Utilities::hmac_sha3_256_from_file(
                track["hmac_key"].get<std::string>(), file_path);
            track["file_size"] = Utilities::get_file_size(file_path);

            // Clean up temporary fields
            track.erase("filename");
            track.erase("max_value");

            // Store in database
            std::string track_str = track.dump(4);
            auto track_blob = ZstdCompressor::compress(track_str);
            (void)database.insert(
                "INSERT OR REPLACE INTO tracks (track_id, track) VALUES (?, ?)",
                {track["track_id"].get<int>(), track_blob});

            auto end = std::chrono::steady_clock::now();
            double elapsed_time =
                std::chrono::duration<double>(end - start).count();
            fmt::print("Successfully processed track {}, took {:.3f} s\n\n",
                       track["track_id"].get<int>(), elapsed_time);
            SPDLOG_TRACE("Successfully processed track {}, took {:.3f} s",
                         track["track_id"].get<int>(), elapsed_time);

        } catch (const std::exception &e) {
            SPDLOG_TRACE("Consumer error: {}", e.what());
            std::exit(EXIT_FAILURE);
        }
    }

    SPDLOG_TRACE("Consumer thread finished");
}

void Migrator::cleanup_file(const fs::path &path) {
    if (fs::exists(path)) {
        try {
            fs::remove(path);
            SPDLOG_TRACE("Removed file: {}", path.string());
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Failed to remove file {}: {}", path.string(),
                         e.what());
        }
    }
}

void Migrator::cleanup_cid_files(const json &track) {
    if (!track.contains("cids")) {
        return;
    }

    MigratorConfig migrator_config(config);
    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const fs::path output_dir = migrator_config.output_dir();

    for (const auto &cid : cids) {
        const fs::path path = output_dir / cid;
        cleanup_file(path);
    }
}