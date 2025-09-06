#include <atomic>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <span>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <spdlog/spdlog.h>
#include <zstd.h>

#include "curl.hpp"
#include "httpvfs.hpp"

class HttpClient {
  public:
    inline static std::atomic<long long> total_downloaded_bytes{0};

    static std::optional<std::vector<char>>
    fetch_range(const std::string &url, sqlite3_int64 offset, int size) {
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
            if (curl.perform() != CURLE_OK)
                return std::nullopt;

            auto data = curl.get_response();
            const auto downloaded = data.size();
            total_downloaded_bytes += downloaded;

            SPDLOG_TRACE(
                "Downloaded {} bytes from range {}-{} | Total downloaded: {} bytes",
                downloaded, offset, offset + size - 1,
                total_downloaded_bytes.load());

            return std::vector<char>(data.begin(), data.end());
        } catch (const std::exception &e) {
            std::cerr << "HTTP fetch error: " << e.what() << "\n";
            return std::nullopt;
        } catch (...) {
            std::cerr << "HTTP fetch error: unknown exception\n";
            return std::nullopt;
        }
    }

    static std::optional<sqlite3_int64>
    get_content_length(const std::string &url) {
        try {
            Curl curl;
            curl.set_option(CURLOPT_URL, url);
            curl.set_option(CURLOPT_NOBODY, 1L);
            curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
            curl.set_option(CURLOPT_MAXREDIRS, 5L);
            curl.set_option(CURLOPT_TIMEOUT, 30L);
            curl.set_option(CURLOPT_CONNECTTIMEOUT, 30L);
            if (curl.perform() != CURLE_OK)
                return std::nullopt;
            long len = curl.get_info<long>(CURLINFO_CONTENT_LENGTH_DOWNLOAD_T);
            return len >= 0 ? std::optional<sqlite3_int64>(len) : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
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
    explicit HttpFile(std::string url)
        : url(std::move(url)), page_cache(MAX_CACHE_PAGES) {
        init_page_size();
    }
    std::optional<std::span<const char>> read_page(sqlite3_int64 page_no) {
        if (auto cached = page_cache.get(page_no))
            return std::span<const char>(cached->get());
        auto page_data =
            HttpClient::fetch_range(url, page_no * page_size, page_size);
        if (!page_data || page_data->empty())
            return std::nullopt;
        page_cache.put(page_no, std::move(*page_data));
        if (auto cached = page_cache.get(page_no))
            return std::span<const char>(cached->get());
        return std::nullopt;
    }
    std::optional<sqlite3_int64> get_file_size() {
        if (!file_size)
            file_size = HttpClient::get_content_length(url);
        return file_size;
    }
    int get_page_size() const noexcept {
        return page_size;
    }

  private:
    void init_page_size() {
        auto buf = HttpClient::fetch_range(url, 0, PREFETCH_SIZE);
        if (!buf || buf->size() < 100) {
            page_size = DEFAULT_PAGE_SIZE;
            return;
        }

        // Parse page size
        const auto ps =
            (static_cast<int>((*buf)[16]) << 8) | static_cast<int>((*buf)[17]);
        page_size = (ps == 1)
                        ? 65536
                        : ((ps >= 512 && ps <= 65536) ? ps : DEFAULT_PAGE_SIZE);

        // Slice the buffer into pages and put them into cache
        for (int pg = 0; pg * page_size < static_cast<int>(buf->size()); ++pg) {
            if (pg > 1)
                break; // we only want page 0 and page 1
            const char *start = buf->data() + pg * page_size;
            int len = std::min(page_size,
                               static_cast<int>(buf->size()) - pg * page_size);
            page_cache.put(pg, std::vector<char>(start, start + len));
        }

        // If page_size is bigger than what we fetched, refetch page 0 fully
        if (page_size > static_cast<int>(buf->size())) {
            auto full_page0 = HttpClient::fetch_range(url, 0, page_size);
            if (full_page0)
                page_cache.put(0, std::move(*full_page0));
        }
    }
    std::string url;
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
};

// Helper function to safely cast sqlite3_file to HttpFileHandle
static HttpFileHandle *get_handle(sqlite3_file *pFile) {
    return reinterpret_cast<HttpFileHandle *>(pFile);
}

extern "C" {

static int httpvfs_xClose(sqlite3_file *pFile) {
    if (!pFile)
        return SQLITE_OK;

    auto *handle = get_handle(pFile);

    // Reset unique_ptrs will automatically clean up
    handle->http_file.reset();
    handle->methods.reset();

    return SQLITE_OK;
}

static int httpvfs_xRead(sqlite3_file *pFile, void *zBuf, int iAmt,
                         sqlite3_int64 iOfst) {
    if (!pFile || !zBuf || iAmt < 0) {
        return SQLITE_IOERR;
    }

    auto *handle = get_handle(pFile);

    if (!handle->http_file) {
        return SQLITE_IOERR;
    }

    const int page_size = handle->http_file->get_page_size();
    const sqlite3_int64 page_no = iOfst / page_size;
    const sqlite3_int64 page_offset = page_no * page_size;
    const int offset_in_page = static_cast<int>(iOfst - page_offset);

    auto page_data = handle->http_file->read_page(page_no);
    if (!page_data || page_data->empty()) {
        return SQLITE_IOERR;
    }

    const int available_bytes =
        static_cast<int>(page_data->size()) - offset_in_page;
    const int copy_size = std::min(iAmt, std::max(0, available_bytes));

    if (copy_size > 0) {
        std::memcpy(zBuf, page_data->data() + offset_in_page, copy_size);
    }

    if (copy_size < iAmt) {
        // Zero-fill remaining bytes
        std::memset(static_cast<char *>(zBuf) + copy_size, 0, iAmt - copy_size);
        return SQLITE_IOERR_SHORT_READ;
    }

    return SQLITE_OK;
}

static int httpvfs_xFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize) {
    if (!pFile || !pSize) {
        return SQLITE_IOERR;
    }

    auto *handle = get_handle(pFile);

    if (!handle->http_file) {
        return SQLITE_IOERR;
    }

    auto size = handle->http_file->get_file_size();
    if (!size) {
        return SQLITE_IOERR;
    }

    *pSize = *size;
    return SQLITE_OK;
}

// Required stub implementations for VFS functions
static int httpvfs_xLock(sqlite3_file *pFile, int eLock) {
    (void)pFile;
    (void)eLock;
    return SQLITE_OK; // Read-only, no locking needed
}

static int httpvfs_xUnlock(sqlite3_file *pFile, int eLock) {
    (void)pFile;
    (void)eLock;
    return SQLITE_OK; // Read-only, no locking needed
}

static int httpvfs_xCheckReservedLock(sqlite3_file *pFile, int *pResOut) {
    (void)pFile;
    if (pResOut)
        *pResOut = 0; // Never locked
    return SQLITE_OK;
}

static int httpvfs_xFileControl(sqlite3_file *pFile, int op, void *pArg) {
    (void)pFile;
    (void)op;
    (void)pArg;
    return SQLITE_NOTFOUND; // No special operations supported
}

static int httpvfs_xSectorSize(sqlite3_file *pFile) {
    (void)pFile;
    return 4096; // Default sector size
}

static int httpvfs_xDeviceCharacteristics(sqlite3_file *pFile) {
    (void)pFile;
    return SQLITE_IOCAP_IMMUTABLE; // HTTP files are immutable
}

static int httpvfs_xOpen(sqlite3_vfs *vfs, const char *zName,
                         sqlite3_file *file, int flags, int *pOutFlags) {

    if (!zName || !file) {
        return SQLITE_IOERR;
    }

    // Only allow read-only access
    if (!(flags & SQLITE_OPEN_READONLY)) {
        return SQLITE_CANTOPEN;
    }

    auto *handle = get_handle(file);
    std::memset(handle, 0, sizeof(HttpFileHandle)); // Zero-initialize

    try {
        // Create IO methods with all required functions using designated
        // initializers
        handle->methods = std::make_unique<sqlite3_io_methods>();
        *(handle->methods) = {.iVersion = 1,
                              .xClose = httpvfs_xClose,
                              .xRead = httpvfs_xRead,
                              .xWrite = nullptr,    // Read-only
                              .xTruncate = nullptr, // Read-only
                              .xSync = nullptr,     // Read-only
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
        handle->http_file = std::make_unique<HttpFile>(zName);

        if (pOutFlags) {
            *pOutFlags = flags;
        }

        return SQLITE_OK;

    } catch (...) {
        return SQLITE_IOERR;
    }
}

// VFS-level functions
static int httpvfs_xAccess(sqlite3_vfs *vfs, const char *zName, int flags,
                           int *pResOut) {
    (void)vfs;
    (void)zName;
    (void)flags;
    if (pResOut)
        *pResOut = 1; // Always assume file exists (will fail on open if not)
    return SQLITE_OK;
}

static int httpvfs_xFullPathname(sqlite3_vfs *vfs, const char *zName, int nOut,
                                 char *zOut) {
    (void)vfs;
    if (!zName || !zOut || nOut <= 0) {
        return SQLITE_CANTOPEN;
    }
    int len = std::strlen(zName);
    if (len >= nOut) {
        return SQLITE_CANTOPEN;
    }
    std::strcpy(zOut, zName);
    return SQLITE_OK;
}

} // extern "C"

// Initialize VFS structure properly with designated initializers
static sqlite3_vfs http_vfs = {
    .iVersion = 1,
    .szOsFile = sizeof(HttpFileHandle),
    .mxPathname = 1024,
    .pNext = nullptr,
    .zName = "httpvfs",
    .pAppData = nullptr,
    .xOpen = httpvfs_xOpen,
    .xDelete = nullptr, // Read-only VFS
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

SQLiteDB::SQLiteDB(const std::string &url, const char *vfs_name) : db(nullptr) {
    int rc = sqlite3_open_v2(url.c_str(), &db, SQLITE_OPEN_READONLY, vfs_name);
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
