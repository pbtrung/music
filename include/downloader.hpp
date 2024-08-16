#ifndef DOWNLOADER_HPP
#define DOWNLOADER_HPP

#include "database.hpp"
#include "json.hpp"
#include <spdlog/spdlog.h>
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
    size_t getNumCIDs() const;
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
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<spdlog::logger> file_logger;
};

class Downloader {
  public:
    Downloader(const json &config, const Database &db);

    void performDownloads();
    void assembleFiles();
    std::vector<FileInfo> getFileInfo() const;

  private:
    size_t getNumThreads() const;

    const json &config;
    const Database &db;
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<spdlog::logger> file_logger;
    std::vector<std::unique_ptr<FileDownloader>> fileDownloaders;
};

#endif // DOWNLOADER_HPP
