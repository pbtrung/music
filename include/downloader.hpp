#ifndef DOWNLOADER_HPP
#define DOWNLOADER_HPP

#include "database.hpp"
#include "json.hpp"
#include "thread_pool.hpp" // Include your thread pool header
#include <curl/curl.h>
#include <string>
#include <vector>

using json = nlohmann::json;

enum class DownloadStatus { Pending, Succeeded, Failed };

struct FileInfo {
    std::string filename;
    std::string extension;
    std::string albumPath;
    std::string trackName;
};

struct DownloadInfo {
    std::string cid;
    DownloadStatus *cidDownloadStatus;
    json config;
};

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

class FileDownloader {
  public:
    FileDownloader(const std::string &filename, const std::string &albumPath,
                   const std::string &trackName, const std::string &extension,
                   const std::vector<std::string> &cids, const json &config);

    void assemble();
    std::string getFilename() const;
    std::string getExtension() const;
    DownloadStatus getFileDownloadStatus() const;
    std::string getAlbumPath() const;
    std::string getTrackName() const;
    std::vector<std::string> getCIDs() const;
    void downloadCid(const DownloadInfo &downloadInfo);

    std::vector<DownloadStatus> cidDownloadStatus;
  private:
    std::string filename;
    std::string albumPath;
    std::string trackName;
    std::string extension;
    std::vector<std::string> cids;
    json config;
    DownloadStatus fileDownloadStatus;
};

class Downloader {
  public:
    Downloader(const json &config, const Database &db);

    void performDownloads();
    void assembleFiles();
    std::vector<FileInfo> getFileInfo() const;

  private:
    json config;
    Database db;
    std::vector<std::unique_ptr<FileDownloader>> fileDownloaders;
};

#endif // DOWNLOADER_HPP
