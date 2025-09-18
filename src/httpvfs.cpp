#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <list>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>
#include <zstd.h>

#include "curl.hpp"
#include "httpvfs.hpp"

class HttpClient {
  private:
    static void backoff_with_jitter(int attempt, int base_ms = 100) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        int max_delay = base_ms * (1 << attempt);
        std::uniform_int_distribution<int> dist(0, max_delay);
        int delay = dist(rng);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

  public:
    inline static std::atomic<long long> total_downloaded_bytes{0};

    static std::optional<std::vector<char>>
    fetch_range(const std::string &url, sqlite3_int64 offset, int size,
                const nlohmann::json &config = {}) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            try {
                Curl curl;
                std::ostringstream oss;
                oss << offset << "-" << (offset + size - 1);
                curl.set_option(CURLOPT_URL, url);
                curl.set_option(CURLOPT_RANGE, oss.str());
                curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
                curl.set_option(CURLOPT_MAXREDIRS, 5L);
                curl.set_option(CURLOPT_TIMEOUT, 30L);
                curl.set_option(CURLOPT_CONNECTTIMEOUT, 30L);

                curl.set_option(CURLOPT_HTTPGET, 1L);
                curl.set_option(CURLOPT_AWS_SIGV4, "aws:amz:auto:s3");
                curl.set_header("x-amz-content-sha256: UNSIGNED-PAYLOAD");

                // Use credentials from config if provided
                std::string credentials;
                if (!config.empty() && config.contains("r2")) {
                    credentials =
                        config["r2"]["access_key"].get<std::string>() + ":" +
                        config["r2"]["secret_key"].get<std::string>();
                } else {
                    throw std::runtime_error("Failed to load r2 credentials");
                }

                curl.set_option(CURLOPT_USERPWD, credentials.c_str());

                if (curl.perform() == CURLE_OK) {
                    auto data = curl.get_response();
                    const auto downloaded = data.size();
                    total_downloaded_bytes += downloaded;

                    SPDLOG_TRACE(
                        "Downloaded {} bytes from range {}-{} | Total downloaded: {} bytes",
                        downloaded, offset, offset + size - 1,
                        total_downloaded_bytes.load());

                    return std::vector<char>(data.begin(), data.end());
                }
            } catch (const std::exception &e) {
                SPDLOG_TRACE("HTTP fetch error: {} (attempt {})", e.what(),
                             attempt + 1);
            } catch (...) {
                SPDLOG_TRACE("HTTP fetch error: unknown exception (attempt {})",
                             attempt + 1);
            }

            backoff_with_jitter(attempt);
        }
        return std::nullopt;
    }
};

template <typename Key, typename Value> class LRUCache {
  public:
    explicit LRUCache(size_t max_size) : max_size(max_size) {}

    void put(const Key &key, Value &&value) {
        std::lock_guard lock(mutex);
        if (auto it = map.find(key); it != map.end()) {
            list.splice(list.begin(), list, it->second);
            it->second->second = std::move(value);
            return;
        }
        list.emplace_front(key, std::move(value));
        map[key] = list.begin();
        if (list.size() > max_size) {
            auto last = std::prev(list.end());
            map.erase(last->first);
            list.pop_back();
        }
    }

    std::optional<std::reference_wrapper<Value>> get(const Key &key) {
        std::lock_guard lock(mutex);
        if (auto it = map.find(key); it != map.end()) {
            list.splice(list.begin(), list, it->second);
            return std::ref(it->second->second);
        }
        return std::nullopt;
    }

  private:
    using ListType = std::list<std::pair<Key, Value>>;
    using MapType = std::unordered_map<Key, typename ListType::iterator>;
    size_t max_size;
    ListType list;
    MapType map;
    mutable std::mutex mutex;
};

class HttpFile {
  public:
    explicit HttpFile(std::string url, const nlohmann::json &config = {})
        : url(std::move(url)), config(config), page_cache(MAX_CACHE_PAGES) {
        init_page_size();
    }

    std::optional<std::span<const char>> read_page(sqlite3_int64 page_no) {
        if (auto cached = page_cache.get(page_no))
            return std::span<const char>(cached->get());

        auto page_data = HttpClient::fetch_range(url, page_no * page_size,
                                                 page_size, config);
        if (!page_data || page_data->empty())
            return std::nullopt;

        page_cache.put(page_no, std::move(*page_data));
        if (auto cached = page_cache.get(page_no))
            return std::span<const char>(cached->get());
        return std::nullopt;
    }

    std::optional<sqlite3_int64> get_file_size() {
        if (file_size)
            return file_size;

        // Try to get file size from SQLite database header first
        auto page0 = read_page(0);
        if (page0 && page0->size() >= 32) {
            const unsigned char *data =
                reinterpret_cast<const unsigned char *>(page0->data());

            // Read page count (4 bytes, big-endian, at offset 28)
            sqlite3_int64 page_count =
                (static_cast<sqlite3_int64>(data[28]) << 24) |
                (static_cast<sqlite3_int64>(data[29]) << 16) |
                (static_cast<sqlite3_int64>(data[30]) << 8) |
                static_cast<sqlite3_int64>(data[31]);

            if (page_count > 0) {
                file_size = page_count * page_size;
                return file_size;
            }
        }

        // Fallback to zero
        file_size = 0;
        return file_size;
    }

    int get_page_size() const noexcept {
        return page_size;
    }

  private:
    void init_page_size() {
        auto buf = HttpClient::fetch_range(url, 0, PREFETCH_SIZE, config);
        if (!buf || buf->size() < 100) {
            page_size = DEFAULT_PAGE_SIZE;
            return;
        }

        // Parse page size from SQLite header
        const auto ps =
            (static_cast<int>((*buf)[16]) << 8) | static_cast<int>((*buf)[17]);
        page_size = (ps == 1)
                        ? 65536
                        : ((ps >= 512 && ps <= 65536) ? ps : DEFAULT_PAGE_SIZE);

        // Cache initial pages
        for (int pg = 0; pg * page_size < static_cast<int>(buf->size()); ++pg) {
            if (pg > 1)
                break; // Only cache page 0 and page 1

            const char *start = buf->data() + pg * page_size;
            int len = std::min(page_size,
                               static_cast<int>(buf->size()) - pg * page_size);
            page_cache.put(pg, std::vector<char>(start, start + len));
        }

        // Refetch page 0 if needed
        if (page_size > static_cast<int>(buf->size())) {
            auto full_page0 =
                HttpClient::fetch_range(url, 0, page_size, config);
            if (full_page0)
                page_cache.put(0, std::move(*full_page0));
        }
    }

    std::string url;
    nlohmann::json config;
    int page_size = DEFAULT_PAGE_SIZE;
    std::optional<sqlite3_int64> file_size;
    LRUCache<sqlite3_int64, std::vector<char>> page_cache;
    static constexpr size_t MAX_CACHE_PAGES = 32;
    static constexpr int DEFAULT_PAGE_SIZE = 4096;
    static constexpr int PREFETCH_SIZE = 16384;
};

struct HttpFileHandle {
    sqlite3_file base{};
    std::unique_ptr<HttpFile> http_file;
    std::unique_ptr<sqlite3_io_methods> methods;
    nlohmann::json config;
};

static HttpFileHandle *get_handle(sqlite3_file *pFile) {
    return reinterpret_cast<HttpFileHandle *>(pFile);
}

extern "C" {

static int httpvfs_xClose(sqlite3_file *pFile) {
    if (!pFile)
        return SQLITE_OK;

    auto *handle = get_handle(pFile);
    handle->http_file.reset();
    handle->methods.reset();
    return SQLITE_OK;
}

static int httpvfs_xRead(sqlite3_file *pFile, void *zBuf, int iAmt,
                         sqlite3_int64 iOfst) {
    if (!pFile || !zBuf || iAmt < 0)
        return SQLITE_IOERR;

    auto *handle = get_handle(pFile);
    if (!handle->http_file)
        return SQLITE_IOERR;

    const int page_size = handle->http_file->get_page_size();
    char *output = static_cast<char *>(zBuf);
    int bytes_read = 0;

    while (bytes_read < iAmt) {
        const sqlite3_int64 current_offset = iOfst + bytes_read;
        const sqlite3_int64 page_no = current_offset / page_size;
        const int offset_in_page = static_cast<int>(current_offset % page_size);

        auto page_data = handle->http_file->read_page(page_no);
        if (!page_data || page_data->empty()) {
            if (bytes_read < iAmt) {
                std::memset(output + bytes_read, 0, iAmt - bytes_read);
                return bytes_read > 0 ? SQLITE_IOERR_SHORT_READ : SQLITE_IOERR;
            }
            break;
        }

        const int available_in_page =
            static_cast<int>(page_data->size()) - offset_in_page;
        const int remaining_bytes = iAmt - bytes_read;
        const int copy_bytes = std::min(available_in_page, remaining_bytes);

        if (copy_bytes <= 0)
            break;

        std::memcpy(output + bytes_read, page_data->data() + offset_in_page,
                    copy_bytes);
        bytes_read += copy_bytes;
    }

    if (bytes_read < iAmt) {
        std::memset(output + bytes_read, 0, iAmt - bytes_read);
        return bytes_read > 0 ? SQLITE_IOERR_SHORT_READ : SQLITE_IOERR;
    }

    return SQLITE_OK;
}

static int httpvfs_xFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize) {
    if (!pFile || !pSize)
        return SQLITE_IOERR;

    auto *handle = get_handle(pFile);
    if (!handle->http_file)
        return SQLITE_IOERR;

    auto size = handle->http_file->get_file_size();
    if (!size)
        return SQLITE_IOERR;

    *pSize = *size;
    return SQLITE_OK;
}

static int httpvfs_xLock(sqlite3_file *pFile, int eLock) {
    return SQLITE_OK; // Read-only, no locking needed
}

static int httpvfs_xUnlock(sqlite3_file *pFile, int eLock) {
    return SQLITE_OK; // Read-only, no locking needed
}

static int httpvfs_xCheckReservedLock(sqlite3_file *pFile, int *pResOut) {
    if (pResOut)
        *pResOut = 0; // Never locked
    return SQLITE_OK;
}

static int httpvfs_xFileControl(sqlite3_file *pFile, int op, void *pArg) {
    return SQLITE_NOTFOUND; // No special operations supported
}

static int httpvfs_xSectorSize(sqlite3_file *pFile) {
    return 4096; // Default sector size
}

static int httpvfs_xDeviceCharacteristics(sqlite3_file *pFile) {
    return SQLITE_IOCAP_IMMUTABLE; // HTTP files are immutable
}

static int httpvfs_xOpen(sqlite3_vfs *vfs, const char *zName,
                         sqlite3_file *file, int flags, int *pOutFlags) {
    if (!zName || !file)
        return SQLITE_IOERR;

    if (!(flags & SQLITE_OPEN_READONLY))
        return SQLITE_CANTOPEN;

    auto *handle = get_handle(file);
    std::memset(handle, 0, sizeof(HttpFileHandle));

    try {
        handle->methods = std::make_unique<sqlite3_io_methods>();
        *(handle->methods) = {.iVersion = 1,
                              .xClose = httpvfs_xClose,
                              .xRead = httpvfs_xRead,
                              .xWrite = nullptr,
                              .xTruncate = nullptr,
                              .xSync = nullptr,
                              .xFileSize = httpvfs_xFileSize,
                              .xLock = httpvfs_xLock,
                              .xUnlock = httpvfs_xUnlock,
                              .xCheckReservedLock = httpvfs_xCheckReservedLock,
                              .xFileControl = httpvfs_xFileControl,
                              .xSectorSize = httpvfs_xSectorSize,
                              .xDeviceCharacteristics =
                                  httpvfs_xDeviceCharacteristics,
                              .xShmMap = nullptr,
                              .xShmLock = nullptr,
                              .xShmBarrier = nullptr,
                              .xShmUnmap = nullptr,
                              .xFetch = nullptr,
                              .xUnfetch = nullptr};

        handle->base.pMethods = handle->methods.get();

        // Get config from vfs pAppData if available
        nlohmann::json config;
        if (vfs->pAppData) {
            config = *static_cast<nlohmann::json *>(vfs->pAppData);
        }

        handle->config = config;
        handle->http_file = std::make_unique<HttpFile>(zName, config);

        // Validate SQLite header
        auto page0 = handle->http_file->read_page(0);
        if (!page0 || page0->empty() || page0->size() < 100)
            return SQLITE_CANTOPEN;

        std::string header(page0->data(), std::min(16ul, page0->size()));
        if (header.substr(0, 6) != "SQLite")
            return SQLITE_CANTOPEN;

        // Verify file size
        auto file_size = handle->http_file->get_file_size();
        if (!file_size || *file_size <= 0)
            return SQLITE_CANTOPEN;

        if (pOutFlags)
            *pOutFlags = flags;

        return SQLITE_OK;

    } catch (...) {
        return SQLITE_IOERR;
    }
}

static int httpvfs_xAccess(sqlite3_vfs *vfs, const char *zName, int flags,
                           int *pResOut) {
    if (pResOut)
        *pResOut = 1; // Always assume file exists
    return SQLITE_OK;
}

static int httpvfs_xFullPathname(sqlite3_vfs *vfs, const char *zName, int nOut,
                                 char *zOut) {
    if (!zName || !zOut || nOut <= 0)
        return SQLITE_CANTOPEN;

    int len = std::strlen(zName);
    if (len >= nOut)
        return SQLITE_CANTOPEN;

    std::strcpy(zOut, zName);
    return SQLITE_OK;
}

} // extern "C"

static sqlite3_vfs http_vfs = {
    .iVersion = 1,
    .szOsFile = sizeof(HttpFileHandle),
    .mxPathname = 1024,
    .pNext = nullptr,
    .zName = "httpvfs",
    .pAppData = nullptr,
    .xOpen = httpvfs_xOpen,
    .xDelete = nullptr,
    .xAccess = httpvfs_xAccess,
    .xFullPathname = httpvfs_xFullPathname,
    .xDlOpen = nullptr,
    .xDlError = nullptr,
    .xDlSym = nullptr,
    .xDlClose = nullptr,
    .xRandomness = nullptr,
    .xSleep = nullptr,
    .xCurrentTime = nullptr,
    .xGetLastError = nullptr,
    .xCurrentTimeInt64 = nullptr,
    .xSetSystemCall = nullptr,
    .xGetSystemCall = nullptr,
    .xNextSystemCall = nullptr,
};

int register_http_vfs() {
    return sqlite3_vfs_register(&http_vfs, 0);
}

SQLiteDB::SQLiteDB(const std::string &url, const nlohmann::json &config,
                   const char *vfs_name = "httpvfs")
    : db(nullptr), config(config) {

    // Create a copy of the http_vfs and set the config in pAppData
    custom_vfs = http_vfs;
    custom_vfs.pAppData = &this->config;

    // Register the custom VFS with a unique name
    std::string custom_vfs_name =
        std::string(vfs_name) + "_" +
        std::to_string(reinterpret_cast<uintptr_t>(this));
    custom_vfs.zName = custom_vfs_name.c_str();

    int reg_result = sqlite3_vfs_register(&custom_vfs, 0);
    if (reg_result != SQLITE_OK) {
        throw std::runtime_error("Failed to register custom VFS");
    }

    int rc = sqlite3_open_v2(url.c_str(), &db, SQLITE_OPEN_READONLY,
                             custom_vfs_name.c_str());
    if (rc != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "Unknown error";
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
        throw std::runtime_error("Failed to open DB: " + err);
    }
}

SQLiteDB::~SQLiteDB() {
    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }
}

SQLiteDB::SQLiteDB(SQLiteDB &&o) noexcept : db(std::exchange(o.db, nullptr)) {}

SQLiteDB &SQLiteDB::operator=(SQLiteDB &&o) noexcept {
    if (this != &o) {
        if (db) {
            sqlite3_close(db);
        }
        db = std::exchange(o.db, nullptr);
    }
    return *this;
}

sqlite3 *SQLiteDB::get() const noexcept {
    return db;
}

SQLiteStmt::SQLiteStmt(sqlite3 *db, const std::string &query) : stmt(nullptr) {
    if (!db) {
        throw std::runtime_error("Database handle is null");
    }
    int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Query preparation error: " +
                                 std::string(sqlite3_errmsg(db)));
    }
}

SQLiteStmt::~SQLiteStmt() {
    if (stmt) {
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
}

SQLiteStmt::SQLiteStmt(SQLiteStmt &&o) noexcept
    : stmt(std::exchange(o.stmt, nullptr)) {}

SQLiteStmt &SQLiteStmt::operator=(SQLiteStmt &&o) noexcept {
    if (this != &o) {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
        stmt = std::exchange(o.stmt, nullptr);
    }
    return *this;
}

sqlite3_stmt *SQLiteStmt::get() const noexcept {
    return stmt;
}

std::vector<char> ZstdDecompressor::decompress(const void *data, size_t size) {
    if (!data || size == 0) {
        throw std::runtime_error("Invalid input data");
    }

    auto dsize = ZSTD_getFrameContentSize(data, size);
    if (dsize == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("Invalid zstd frame");
    }
    if (dsize == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("Unknown decompressed size");
    }
    if (dsize > SIZE_MAX) {
        throw std::runtime_error("Decompressed size too large");
    }

    std::vector<char> out(static_cast<size_t>(dsize));
    size_t r = ZSTD_decompress(out.data(), out.size(), data, size);
    if (ZSTD_isError(r)) {
        throw std::runtime_error("Zstd error: " +
                                 std::string(ZSTD_getErrorName(r)));
    }

    return out;
}

std::vector<std::byte> ZstdCompressor::compress(const void *data, size_t size,
                                                int compression_level) {
    if (!data || size == 0) {
        throw std::runtime_error("Invalid input data");
    }

    // Get the maximum compressed size bound
    size_t max_compressed_size = ZSTD_compressBound(size);
    if (ZSTD_isError(max_compressed_size)) {
        throw std::runtime_error("Failed to get compression bound");
    }

    std::vector<std::byte> out(max_compressed_size);

    // Compress the data
    size_t compressed_size =
        ZSTD_compress(out.data(), out.size(), data, size, compression_level);

    if (ZSTD_isError(compressed_size)) {
        throw std::runtime_error(
            "Zstd compression error: " +
            std::string(ZSTD_getErrorName(compressed_size)));
    }

    // Resize to actual compressed size
    out.resize(compressed_size);
    return out;
}

std::vector<std::byte>
ZstdCompressor::compress(const std::vector<std::byte> &input,
                         int compression_level) {
    return compress(input.data(), input.size(), compression_level);
}

std::vector<std::byte> ZstdCompressor::compress(const std::vector<char> &input,
                                                int compression_level) {
    return compress(input.data(), input.size(), compression_level);
}

std::vector<std::byte> ZstdCompressor::compress(const std::string &input,
                                                int compression_level) {
    return compress(input.data(), input.size(), compression_level);
}
