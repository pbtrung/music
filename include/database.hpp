#ifndef DATABASE_HPP
#define DATABASE_HPP

#include <fmt/core.h>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class Database {
  public:
    explicit Database(std::string_view filename);
    ~Database();

    int countTracks() const;
    std::vector<std::string> getTrackCIDs(int trackId) const;
    std::string getTrackName(int trackId) const;
    std::string getAlbumPath(int trackId) const;

  private:
    class SqliteStatement {
      public:
        SqliteStatement(sqlite3 *db, std::string_view query);
        ~SqliteStatement();
        sqlite3_stmt *get() const;

      private:
        sqlite3_stmt *stmt = nullptr;
    };

    sqlite3 *db = nullptr;
};

#endif // DATABASE_HPP
