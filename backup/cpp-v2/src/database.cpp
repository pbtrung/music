#include "database.hpp"
#include <fmt/core.h>
#include <stdexcept>

Database::Database(std::string_view filename) {
    logger = spdlog::get("logger");
    int rc =
        sqlite3_open_v2(filename.data(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to open database: {}",
                            sqlite3_errmsg(db));
        throw std::runtime_error("");
    }
}

Database::~Database() {
    if (db) {
        sqlite3_close(db);
    }
}

Database::SqliteStatement::SqliteStatement(sqlite3 *db,
                                           std::string_view query) {
    std::shared_ptr<spdlog::logger> logger = spdlog::get("logger");
    if (sqlite3_prepare_v2(db, query.data(), -1, &stmt, nullptr) != SQLITE_OK) {
        SPDLOG_LOGGER_ERROR(logger, "Error: SQL error: {}", sqlite3_errmsg(db));
        throw std::runtime_error("");
    }
}

Database::SqliteStatement::~SqliteStatement() {
    sqlite3_finalize(stmt);
}

sqlite3_stmt *Database::SqliteStatement::get() const {
    return stmt;
}

int Database::countTracks() const {
    const std::string_view query = "SELECT count(*) FROM tracks";
    int num_tracks = 0;

    SqliteStatement stmt(db, query);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        num_tracks = sqlite3_column_int(stmt.get(), 0);
    }
    if (num_tracks == 0) {
        const char *errmsg = sqlite3_errmsg(db);
        if (errmsg) {
            SPDLOG_LOGGER_ERROR(logger, "Error: num_tracks == 0: {}", errmsg);
            throw std::runtime_error("");
        } else {
            SPDLOG_LOGGER_ERROR(logger,
                                "Error: num_tracks == 0: <no error message>");
            throw std::runtime_error("");
        }
    }

    return num_tracks;
}

std::vector<std::string> Database::getTrackCIDs(int trackId) const {
    const std::string_view query = "SELECT cid FROM content WHERE track_id = ?";

    std::vector<std::string> cids;
    SqliteStatement stmt(db, query);
    sqlite3_bind_int(stmt.get(), 1, trackId);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const unsigned char *cid = sqlite3_column_text(stmt.get(), 0);
        cids.emplace_back(reinterpret_cast<const char *>(cid));
    }
    if (cids.size() == 0) {
        const char *errmsg = sqlite3_errmsg(db);
        if (errmsg) {
            SPDLOG_LOGGER_ERROR(logger,
                                "Error: cids.size() == 0: trackId: {} {}",
                                trackId, errmsg);
            throw std::runtime_error("");
        } else {
            SPDLOG_LOGGER_ERROR(
                logger,
                "Error: cids.size() == 0: trackId: {} <no error message>",
                trackId);
            throw std::runtime_error("");
        }
    }
    return cids;
}

std::string Database::getTrackName(int trackId) const {
    const std::string_view query =
        "SELECT track_name FROM tracks WHERE track_id = ?";

    SqliteStatement stmt(db, query);
    sqlite3_bind_int(stmt.get(), 1, trackId);

    const unsigned char *name = nullptr;
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        name = sqlite3_column_text(stmt.get(), 0);
    }
    if (name == nullptr) {
        const char *errmsg = sqlite3_errmsg(db);
        if (errmsg) {
            SPDLOG_LOGGER_ERROR(logger,
                                "Error: name == nullptr: trackId: {} {}",
                                trackId, errmsg);
            throw std::runtime_error("");
        } else {
            SPDLOG_LOGGER_ERROR(
                logger,
                "Error: name == nullptr: trackId: {} <no error message>",
                trackId);
            throw std::runtime_error("");
        }
    }
    return reinterpret_cast<const char *>(name);
}

std::string Database::getAlbumPath(int trackId) const {
    const std::string_view query =
        "SELECT path FROM albums WHERE album_id = (SELECT album_id FROM tracks "
        "WHERE track_id = ?)";

    SqliteStatement stmt(db, query);
    sqlite3_bind_int(stmt.get(), 1, trackId);

    const unsigned char *path = nullptr;
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        path = sqlite3_column_text(stmt.get(), 0);
    }
    if (path == nullptr) {
        const char *errmsg = sqlite3_errmsg(db);
        if (errmsg) {
            SPDLOG_LOGGER_ERROR(logger,
                                "Error: path == nullptr: trackId: {} {}",
                                trackId, errmsg);
            throw std::runtime_error("");
        } else {
            SPDLOG_LOGGER_ERROR(
                logger,
                "Error: path == nullptr: trackId: {} <no error message>",
                trackId);
            throw std::runtime_error("");
        }
    }

    return reinterpret_cast<const char *>(path);
}