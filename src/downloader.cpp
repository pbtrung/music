#include <spdlog/spdlog.h>

#include "downloader.hpp"

Downloader::Downloader(const nlohmann::json &track) : track(track) {}
