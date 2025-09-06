#pragma once

#include "httpvfs.hpp"
#include "json.hpp"

class Track {
  public:
    Track(int id, nlohmann::json json);

    int get_id() const;
    const nlohmann::json get_json() const;
    static Track load(const std::string &url, const std::string &query);

  private:
    int track_id;
    nlohmann::json track_json;
};
