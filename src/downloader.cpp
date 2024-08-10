#include "downloader.hpp"
#include "const.hpp"
#include "random.hpp"
#include "utils.hpp"
#include <algorithm>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <iostream>

typedef struct {
    std::string cid;
    std::string filename;
    DownloadStatus *cidDownloadStatus;
    json config;
} DownloadInfo;

static size_t writeCallback(void *ptr, size_t size, size_t nmemb,
                            void *userdata) {
    auto *outfile = static_cast<std::ofstream *>(userdata);
    outfile->write(static_cast<const char *>(ptr), size * nmemb);
    return size * nmemb;
}

FileDownloader::FileDownloader(const std::string &filename,
                               const std::string &albumPath,
                               const std::string &trackName,
                               const std::string &ext,
                               const std::vector<std::string> &cids,
                               const json &config)
    : filename(filename), albumPath(albumPath), trackName(trackName),
      extension(ext), cids(cids), config(config) {
    cidDownloadStatus.resize(cids.size(), DownloadStatus::Pending);
    fileDownloadStatus = DownloadStatus::Pending;
}

void FileDownloader::downloadCid(uv_work_t *req) {
    auto *downloadInfo = static_cast<DownloadInfo *>(req->data);
    fmt::print(stdout, "Downloading {}\n", downloadInfo->cid);
    std::cout.flush();

    std::filesystem::path filePath =
        std::filesystem::path(
            downloadInfo->config["output"].get<std::string>()) /
        downloadInfo->cid;
    std::ofstream outfile(filePath, std::ios::binary);

    if (!outfile.is_open()) {
        fmt::print(stderr, "Failed to open file {}\n", filePath.string());
        *(downloadInfo->cidDownloadStatus) = DownloadStatus::Failed;
        return;
    }

    try {
        CurlHandle curl;

        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &outfile);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);

        long responseCode = 0;
        std::string url;
        const auto &gateways =
            downloadInfo->config["gateways"].get<std::vector<std::string>>();
        int timeout = downloadInfo->config["timeout"].get<int>();
        int maxRetries = downloadInfo->config["max_retries"].get<int>();

        for (int retries = 0; retries < maxRetries; ++retries) {
            if (downloadInfo->cid.size() == 59) {
                url = fmt::format("https://{}.ipfs.nftstorage.link",
                                  downloadInfo->cid);
                curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 2 * timeout);
            } else {
                auto randomIndex =
                    Random::uniqueInts(1, 0, gateways.size() - 1)[0];
                url = fmt::format("https://{}/{}", gateways[randomIndex],
                                  downloadInfo->cid);
                curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);
            }

            curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());

            if (curl_easy_perform(curl.get()) == CURLE_OK) {
                curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE,
                                  &responseCode);
                if (responseCode == 200) {
                    *(downloadInfo->cidDownloadStatus) =
                        DownloadStatus::Succeeded;
                    break;
                }
            }
            outfile.clear();
            outfile.seekp(0, std::ios::beg);
        }

        if (responseCode != 200) {
            fmt::print(stderr, "Download of cid {} failed\n",
                       downloadInfo->cid);
            *(downloadInfo->cidDownloadStatus) = DownloadStatus::Failed;
        }
    } catch (const std::exception &e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        *(downloadInfo->cidDownloadStatus) = DownloadStatus::Failed;
    }
}

void FileDownloader::onCidDownloadCompleted(uv_work_t *req, int status) {
    auto *downloadInfo = static_cast<DownloadInfo *>(req->data);
    delete downloadInfo;
    delete req;
}

void FileDownloader::download(uv_loop_t *loop) {
    for (size_t i = 0; i < cids.size(); ++i) {
        auto *req = new uv_work_t;
        auto *downloadInfo = new DownloadInfo;

        downloadInfo->cid = cids[i];
        downloadInfo->filename = filename;
        downloadInfo->cidDownloadStatus = &cidDownloadStatus[i];
        downloadInfo->config = config;
        req->data = downloadInfo;

        uv_queue_work(loop, req, downloadCid, onCidDownloadCompleted);
    }
}

void FileDownloader::assemble() {
    fileDownloadStatus =
        std::all_of(cidDownloadStatus.begin(), cidDownloadStatus.end(),
                    [](DownloadStatus status) {
                        return status == DownloadStatus::Succeeded;
                    })
            ? DownloadStatus::Succeeded
            : DownloadStatus::Failed;

    if (fileDownloadStatus == DownloadStatus::Succeeded) {
        fmt::print(stdout, "\n");
        fmt::print(stdout, "{:<{}}: {}\n", "Assemble", WIDTH + 2, filename);
        fmt::print(stdout, "  {:<{}}: {}\n", "Path", WIDTH, albumPath);
        fmt::print(stdout, "  {:<{}}: {}\n", "Filename", WIDTH, trackName);
        std::cout.flush();

        std::string output = config["output"].get<std::string>();
        std::filesystem::path filePath =
            std::filesystem::path(output) / std::filesystem::path(filename);
        std::ofstream outfile(filePath, std::ios::binary);
        if (!outfile.is_open()) {
            throw std::runtime_error("Failed to open output file: " +
                                     filePath.string());
        }

        std::vector<char> buffer(4096);

        for (const auto &cid : cids) {
            std::filesystem::path cidPath =
                std::filesystem::path(output) / std::filesystem::path(cid);

            std::ifstream infile(cidPath, std::ios::binary);
            if (!infile.is_open()) {
                throw std::runtime_error("Failed to open input file: " +
                                         cidPath.string());
            }

            while (infile) {
                infile.read(buffer.data(), buffer.size());
                outfile.write(buffer.data(), infile.gcount());
            }

            infile.close();

            if (!std::filesystem::remove(cidPath)) {
                throw std::runtime_error("Failed to delete file: " +
                                         cidPath.string());
            }
        }

        outfile.close();
    }
}

std::string FileDownloader::getFilename() const {
    return filename;
}

std::string FileDownloader::getExtension() const {
    return extension;
}

DownloadStatus FileDownloader::getFileDownloadStatus() const {
    return fileDownloadStatus;
}

std::string FileDownloader::getAlbumPath() const {
    return albumPath;
}

std::string FileDownloader::getTrackName() const {
    return trackName;
}

Downloader::Downloader(const json &config, const Database &db)
    : config(config), db(db) {
    // Get the number of tracks and the minimum value from the config
    int maxValue = db.countTracks();
    int numFiles = config["num_files"].get<int>();
    int minValue = config["min_value"].get<int>();

    // Ensure that we have valid parameters
    if (numFiles <= 0 || minValue < 0 || minValue >= maxValue) {
        throw std::invalid_argument(
            "Invalid parameters for file download configuration.");
    }

    // Generate random indices for selecting tracks
    std::vector<int> randomIndices =
        Random::uniqueInts(numFiles, minValue, maxValue);

    // Create FileDownloader instances
    for (int index : randomIndices) {
        // Retrieve track information from the database
        std::vector<std::string> cids = db.getTrackCIDs(index);
        std::string trackName = db.getTrackName(index);
        std::string albumPath = db.getAlbumPath(index);
        std::string ext = Utils::getExtension(trackName);
        std::string filename = Random::alphanumericString(20) + "." + ext;

        // Add a new FileDownloader to the list
        fileDownloaders.emplace_back(std::make_unique<FileDownloader>(
            filename, albumPath, trackName, ext, cids, config));
    }
}
void Downloader::performDownloads() {
    uv_loop_t *loop = uv_default_loop();
    for (auto &fileDownloader : fileDownloaders) {
        fileDownloader->download(loop);
    }
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
}

void Downloader::assembleFiles() {
    for (auto &fileDownloader : fileDownloaders) {
        fileDownloader->assemble();
    }
}

std::vector<FileInfo> Downloader::getFileInfo() const {
    std::vector<FileInfo> fileInfo;
    fileInfo.reserve(fileDownloaders.size());

    for (const auto &fileDownloader : fileDownloaders) {
        fileInfo.push_back(
            {fileDownloader->getFilename(), fileDownloader->getExtension(),
             fileDownloader->getAlbumPath(), fileDownloader->getTrackName()});
    }
    return fileInfo;
}
