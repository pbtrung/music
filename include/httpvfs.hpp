#pragma once

#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

int register_http_vfs();

class SQLiteDB {
  public:
    explicit SQLiteDB(const std::string &url, const char *vfs_name);
    ~SQLiteDB();
    SQLiteDB(const SQLiteDB &) = delete;
    SQLiteDB &operator=(const SQLiteDB &) = delete;
    SQLiteDB(SQLiteDB &&) noexcept;
    SQLiteDB &operator=(SQLiteDB &&) noexcept;
    sqlite3 *get() const noexcept;

  private:
    sqlite3 *db = nullptr;
};

class SQLiteStmt {
  public:
    SQLiteStmt(sqlite3 *db, const std::string &query);
    ~SQLiteStmt();
    SQLiteStmt(const SQLiteStmt &) = delete;
    SQLiteStmt &operator=(const SQLiteStmt &) = delete;
    SQLiteStmt(SQLiteStmt &&) noexcept;
    SQLiteStmt &operator=(SQLiteStmt &&) noexcept;
    sqlite3_stmt *get() const noexcept;

  private:
    sqlite3_stmt *stmt = nullptr;
};

class ZstdDecompressor {
  public:
    static std::vector<char> decompress(const void *compressed_data,
                                        size_t compressed_size);
};
