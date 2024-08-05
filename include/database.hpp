#ifndef DATABASE_HPP
#define DATABASE_HPP

#include <fmt/core.h>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <vector>

class database {
  public:
    explicit database(const std::string &filename);
    ~database();

    int count_tracks() const;
    std::vector<std::string> get_cids(int track_id) const;
    std::string get_track_name(int track_id) const;
    std::string get_album(int track_id) const;

  private:
    class sqlite_statement {
      public:
        sqlite_statement(sqlite3 *db, const std::string &query);
        ~sqlite_statement();
        sqlite3_stmt *get() const;

      private:
        sqlite3_stmt *stmt = nullptr;
    };

    sqlite3 *db = nullptr;
};

#endif // DATABASE_HPP