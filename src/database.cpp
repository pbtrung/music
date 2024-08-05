#include "database.hpp"

database::database(const std::string &filename) {
    int rc =
        sqlite3_open_v2(filename.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        std::string error_message =
            fmt::format("Cannot open database: {}", sqlite3_errmsg(db));
        sqlite3_close(db);
        throw std::runtime_error(error_message);
    }
}

database::~database() {
    if (db) {
        sqlite3_close(db);
    }
}

database::sqlite_statement::sqlite_statement(sqlite3 *db,
                                             const std::string &query) {
    if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        throw std::runtime_error(
            fmt::format("SQL error: {}", sqlite3_errmsg(db)));
    }
}

database::sqlite_statement::~sqlite_statement() { sqlite3_finalize(stmt); }

sqlite3_stmt *database::sqlite_statement::get() const { return stmt; }

int database::count_tracks() const {
    const std::string query = "SELECT count(*) FROM tracks";
    try {
        sqlite_statement stmt(db, query);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            return sqlite3_column_int(stmt.get(), 0);
        }
    } catch (const std::exception &e) {
        fmt::print("Error: {}\n", e.what());
        return -1;
    }
    return 0;
}

std::vector<std::string> database::get_cids(int track_id) const {
    const std::string query = "SELECT cid FROM content WHERE track_id = ?";
    std::vector<std::string> cids;

    try {
        sqlite_statement stmt(db, query);
        sqlite3_bind_int(stmt.get(), 1, track_id);

        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const unsigned char *cid = sqlite3_column_text(stmt.get(), 0);
            cids.emplace_back(reinterpret_cast<const char *>(cid));
        }
    } catch (const std::exception &e) {
        fmt::print("Error: {}\n", e.what());
    }

    return cids;
}

std::string database::get_track_name(int track_id) const {
    const std::string query =
        "SELECT track_name FROM tracks WHERE track_id = ?";

    try {
        sqlite_statement stmt(db, query);
        sqlite3_bind_int(stmt.get(), 1, track_id);

        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(stmt.get(), 0);
            return reinterpret_cast<const char *>(name);
        }
    } catch (const std::exception &e) {
        fmt::print("Error: {}\n", e.what());
    }

    return "";
}

std::string database::get_album(int track_id) const {
    const std::string query =
        "SELECT path FROM albums WHERE album_id = (SELECT album_id FROM tracks WHERE track_id = ?)";

    try {
        sqlite_statement stmt(db, query);
        sqlite3_bind_int(stmt.get(), 1, track_id);

        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const unsigned char *path = sqlite3_column_text(stmt.get(), 0);
            return reinterpret_cast<const char *>(path);
        }
    } catch (const std::exception &e) {
        fmt::print("Error: {}\n", e.what());
    }

    return "";
}