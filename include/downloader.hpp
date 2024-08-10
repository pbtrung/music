#ifndef DOWNLOADER_HPP
#define DOWNLOADER_HPP

#include "database.hpp"
#include "json.hpp"
#include <curl/curl.h>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

enum class DownloadStatus { Pending, Succeeded, Failed };

class CurlHandle {
  public:
    CurlHandle() : handle(curl_easy_init()) {
        if (!handle) {
            throw std::runtime_error("Failed to initialize CURL handle");
        }
    }

    ~CurlHandle() {
        if (handle) {
            curl_easy_cleanup(handle);
        }
    }

    CURL *get() const {
        return handle;
    }

  private:
    CURL *handle;
};

struct FileInfo {
    std::string filename;
    std::string extension;
    std::string albumPath;
    std::string trackName;
};

class FileDownloader {
  public:
    FileDownloader(std::string_view filename, std::string_view albumPath,
                   std::string_view trackName, std::string_view extension,
                   const std::vector<std::string> &cids,
                   const nlohmann::json &config);

    std::future<void> download();
    void assemble();
    const std::string &getFilename() const;
    const std::string &getExtension() const;
    DownloadStatus getDownloadStatus() const;
    const std::string &getAlbumPath() const;
    const std::string &getTrackName() const;

  private:
    static size_t writeCallback(void *ptr, size_t size, size_t nmemb,
                                void *userdata);
    static size_t headerCallback(void *contents, size_t size, size_t nmemb,
                                 void *userp);
    std::future<void> downloadCid(const std::string &cid, size_t index);
    void performDownload(const std::string &cid, size_t index);
    bool areAllDownloadsSucceeded() const;

    std::string filename;
    std::string albumPath;
    std::string trackName;
    std::string extension;
    std::vector<std::string> cids;
    nlohmann::json config;
    std::vector<DownloadStatus> cidDownloadStatus;
    DownloadStatus downloadStatus = DownloadStatus::Pending;
};

class Downloader {
  public:
    Downloader(const nlohmann::json &config, const Database &db);
    void performDownloads();
    void assembleFiles();
    std::vector<FileInfo> getFileInfo() const;

  private:
    nlohmann::json config;
    Database db;
    std::vector<std::unique_ptr<FileDownloader>> fileDownloaders;
};

#endif // DOWNLOADER_HPP