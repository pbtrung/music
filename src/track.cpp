#include <sstream>
#include <stdexcept>

#include <sqlite3.h>

#include "httpvfs.hpp"
#include "track.hpp"

Track::Track(int id, nlohmann::json json)
    : track_id(id), track_json(std::move(json)) {}

int Track::get_id() const {
    return track_id;
}

const nlohmann::json &Track::get_json() const {
    return track_json;
}

Track Track::load(const std::string &url, const std::string &query) {
    // Register HTTP VFS once
    if (register_http_vfs() != SQLITE_OK) {
        throw std::runtime_error("Failed to register HTTP VFS");
    }

    SQLiteDB db(url, "httpvfs");
    SQLiteStmt stmt(db.get(), query);

    int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt.get(), 0);

        const void *blob_data = sqlite3_column_blob(stmt.get(), 1);
        int blob_size = sqlite3_column_bytes(stmt.get(), 1);

        if (!blob_data || blob_size <= 0) {
            throw std::runtime_error("No blob data found for track");
        }

        auto decompressed = ZstdDecompressor::decompress(blob_data, blob_size);
        std::string json_str(decompressed.begin(), decompressed.end());

        nlohmann::json parsed_json;
        try {
            parsed_json = nlohmann::json::parse(json_str);
        } catch (const std::exception &e) {
            throw std::runtime_error("JSON parse error: " +
                                     std::string(e.what()));
        }

        return Track(id, std::move(parsed_json));
    }

    throw std::runtime_error("No rows returned from query");
}
