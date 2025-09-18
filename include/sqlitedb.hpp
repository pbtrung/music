#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace db {

using Value = std::variant<int, double, std::string, std::vector<std::byte>,
                           std::nullptr_t>;

class SqliteDb {
  public:
    // Constructor opens database connection
    explicit SqliteDb(std::string_view db_path);

    // Destructor automatically closes connection (RAII)
    ~SqliteDb() noexcept;

    // Delete copy constructor and assignment (unique ownership)
    SqliteDb(const SqliteDb &) = delete;
    SqliteDb &operator=(const SqliteDb &) = delete;

    // Move constructor and assignment
    SqliteDb(SqliteDb &&other) noexcept;
    SqliteDb &operator=(SqliteDb &&other) noexcept;

    // Execute INSERT statement with parameters and return number of affected
    // rows
    [[nodiscard]] int insert(std::string_view insert_sql,
                             const std::vector<Value> &params = {});

    // Execute any SQL statement (for CREATE TABLE, etc.)
    void execute(std::string_view sql);

    // Check if database connection is valid
    [[nodiscard]] bool is_open() const noexcept;

    // Get last insert row ID
    [[nodiscard]] long long last_insert_rowid() const noexcept;

  private:
    sqlite3 *db;

    void close() noexcept;
    [[nodiscard]] std::string get_error_message() const;
};

} // namespace db