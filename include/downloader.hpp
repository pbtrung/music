#ifndef DOWNLOADER_HPP
#define DOWNLOADER_HPP

#include "database.hpp"
#include "json.hpp"
#include "thread_pool.hpp"
#include <curl/curl.h>
#include <string>
#include <vector>

using json = nlohmann::json;

enum class DownloadStatus { Pending, Succeeded, Failed };

struct FileInfo {
    const std::string filename;
    const std::string extension;
    const std::string albumPath;
    const std::string trackName;
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
    const std::string getFilename() const;
    const std::string getExtension() const;
    const DownloadStatus getFileDownloadStatus() const;
    const std::string getAlbumPath() const;
    const std::string getTrackName() const;
    int getNumCIDs();
    void downloadCid(int cid_index);

  private:
    const std::string filename;
    const std::string albumPath;
    const std::string trackName;
    const std::string extension;
    const std::vector<std::string> cids;
    const json &config;
    std::vector<DownloadStatus> cidDownloadStatus;
    DownloadStatus fileDownloadStatus;
};

class Downloader {
  public:
    Downloader(const json &config, const Database &db);

    void performDownloads();
    void assembleFiles();
    std::vector<FileInfo> getFileInfo() const;

  private:
    const json &config;
    const Database &db;
    std::vector<std::unique_ptr<FileDownloader>> fileDownloaders;
};

#endif // DOWNLOADER_HPP
