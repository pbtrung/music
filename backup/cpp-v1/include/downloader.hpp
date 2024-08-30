#ifndef DOWNLOADER_HPP
#define DOWNLOADER_HPP

#include "database.hpp"
#include <curl/curl.h>
#include <string>
#include <uv.h>
#include <vector>

#include "json.hpp"
using json = nlohmann::json;

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
    FileDownloader(const std::string &filename, const std::string &albumPath,
                   const std::string &trackName, const std::string &extension,
                   const std::vector<std::string> &cids, const json &config);

    void download(uv_loop_t *loop);
    void assemble();

    std::string getFilename() const;
    std::string getExtension() const;
    DownloadStatus getFileDownloadStatus() const;
    std::string getAlbumPath() const;
    std::string getTrackName() const;

  private:
    static void onCidDownloadCompleted(uv_work_t *req, int status);
    static void downloadCid(uv_work_t *req);

    std::string filename;
    std::string albumPath;
    std::string trackName;
    std::string extension;
    std::vector<std::string> cids;
    json config;
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
    json config;
    Database db;
    std::vector<std::unique_ptr<FileDownloader>> fileDownloaders;
};

#endif // DOWNLOADER_HPP
