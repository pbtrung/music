#pragma once

#include "curl.hpp"
#include "json.hpp"

class Downloader {
  public:
    explicit Downloader(const nlohmann::json &track);
    ~Downloader() = default;

    Downloader(const Downloader &) = delete;
    Downloader &operator = (const Downloader &) = delete;
    Downloader(Downloader &&) = default;
    Downloader &operator = (Downloader &&) = default;

    void download_file();

  private:
    nlohmann::json track;
};
