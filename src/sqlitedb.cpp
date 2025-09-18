#include <sqlite3.h>
#include <utility>

#include "sqlitedb.hpp"

namespace db {

SqliteDb::SqliteDb(std::string_view db_path) : db(nullptr) {
    int result = sqlite3_open(db_path.data(), &db);
    if (result != SQLITE_OK) {
        std::string error_msg = "Failed to open database: ";
        if (db) {
            error_msg += sqlite3_errmsg(db);
            sqlite3_close(db);
        } else {
            error_msg += "Unable to allocate memory";
        }
        db = nullptr;
        throw std::runtime_error(error_msg);
    }
}

SqliteDb::~SqliteDb() noexcept {
    close();
}

SqliteDb::SqliteDb(SqliteDb &&other) noexcept : db(other.db) {
    other.db = nullptr;
}

SqliteDb &SqliteDb::operator=(SqliteDb &&other) noexcept {
    if (this != &other) {
        close();
        db = other.db;
        other.db = nullptr;
    }
    return *this;
}

int SqliteDb::insert(std::string_view insert_sql,
                     const std::vector<Value> &params) {
    if (!db) {
        throw std::runtime_error("Database is not open");
    }

    sqlite3_stmt *stmt = nullptr;
    int result = sqlite3_prepare_v2(db, insert_sql.data(),
                                    static_cast<int>(insert_sql.length()),
                                    &stmt, nullptr);

    if (result != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare statement: " +
                                 get_error_message());
    }

    // RAII for statement cleanup
    auto stmt_deleter = [](sqlite3_stmt *s) {
        if (s)
            sqlite3_finalize(s);
    };
    std::unique_ptr<sqlite3_stmt, decltype(stmt_deleter)> stmt_guard(
        stmt, stmt_deleter);

    // Bind parameters
    for (size_t i = 0; i < params.size(); ++i) {
        int param_index = static_cast<int>(i + 1);

        std::visit(
            [stmt, param_index](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, int>) {
                    sqlite3_bind_int(stmt, param_index, value);
                } else if constexpr (std::is_same_v<T, double>) {
                    sqlite3_bind_double(stmt, param_index, value);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    sqlite3_bind_text(stmt, param_index, value.c_str(), -1,
                                      SQLITE_TRANSIENT);
                } else if constexpr (std::is_same_v<T,
                                                    std::vector<std::byte>>) {
                    sqlite3_bind_blob(stmt, param_index, value.data(),
                                      static_cast<int>(value.size()),
                                      SQLITE_TRANSIENT);
                } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
                    sqlite3_bind_null(stmt, param_index);
                }
            },
            params[i]);
    }

    result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        throw std::runtime_error("Failed to execute INSERT: " +
                                 get_error_message());
    }

    return sqlite3_changes(db);
}

void SqliteDb::execute(std::string_view sql) {
    if (!db) {
        throw std::runtime_error("Database is not open");
    }

    char *error_msg = nullptr;
    int result = sqlite3_exec(db, sql.data(), nullptr, nullptr, &error_msg);

    if (result != SQLITE_OK) {
        std::string error_str = "Failed to execute SQL: ";
        if (error_msg) {
            error_str += error_msg;
            sqlite3_free(error_msg);
        }
        throw std::runtime_error(error_str);
    }
}

bool SqliteDb::is_open() const noexcept {
    return db != nullptr;
}

long long SqliteDb::last_insert_rowid() const noexcept {
    return db ? sqlite3_last_insert_rowid(db) : 0;
}

void SqliteDb::close() noexcept {
    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }
}

std::string SqliteDb::get_error_message() const {
    return db ? sqlite3_errmsg(db) : "Database is not open";
}

} // namespace db