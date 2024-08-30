#include "database.hpp"

Database::Database(std::string_view filename) {
    int rc =
        sqlite3_open_v2(filename.data(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(
            fmt::format("Cannot open database: {}", sqlite3_errmsg(db)));
    }
}

Database::~Database() {
    if (db) {
        sqlite3_close(db);
    }
}

Database::SqliteStatement::SqliteStatement(sqlite3 *db,
                                           std::string_view query) {
    if (sqlite3_prepare_v2(db, query.data(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(
            fmt::format("SQL error: {}", sqlite3_errmsg(db)));
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

    SqliteStatement stmt(db, query);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int(stmt.get(), 0);
    }

    // No rows found or an error occurred
    return 0;
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
    return cids;
}

std::string Database::getTrackName(int trackId) const {
    const std::string_view query =
        "SELECT track_name FROM tracks WHERE track_id = ?";

    SqliteStatement stmt(db, query);
    sqlite3_bind_int(stmt.get(), 1, trackId);

    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt.get(), 0);
        return reinterpret_cast<const char *>(name);
    }
    return "";
}

std::string Database::getAlbumPath(int trackId) const {
    const std::string_view query =
        "SELECT path FROM albums WHERE album_id = (SELECT album_id FROM tracks "
        "WHERE track_id = ?)";

    SqliteStatement stmt(db, query);
    sqlite3_bind_int(stmt.get(), 1, trackId);

    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const unsigned char *path = sqlite3_column_text(stmt.get(), 0);
        return reinterpret_cast<const char *>(path);
    }

    return "";
}