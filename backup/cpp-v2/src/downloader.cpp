#include "downloader.hpp"
#include "const.hpp"
#include "random.hpp"
#include "thread_pool.hpp"
#include "utils.hpp"
#include <algorithm>
#include <array>
#include <curl/curl.h>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

static std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> createCurlHandle() {
    std::shared_ptr<spdlog::logger> logger = spdlog::get("logger");
    auto curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(
        curl_easy_init(), curl_easy_cleanup);
    if (!curl) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to initialize CURL handle");
        throw std::runtime_error("");
    }
    return curl;
}

static size_t writeCallback(void *ptr, size_t size, size_t nmemb,
                            void *userdata) {
    auto *outfile = static_cast<std::ofstream *>(userdata);
    outfile->write(static_cast<const char *>(ptr), size * nmemb);
    outfile->flush();
    return size * nmemb;
}

FileDownloader::FileDownloader(const std::string &filename,
                               const std::string &albumPath,
                               const std::string &trackName,
                               const std::string &extension,
                               const std::vector<std::string> &cids,
                               const json &config)
    : filename(filename), albumPath(albumPath), trackName(trackName),
      extension(extension), cids(std::move(cids)), config(config) {
    cidDownloadStatus.resize(cids.size(), DownloadStatus::Pending);
    fileDownloadStatus = DownloadStatus::Pending;
    logger = spdlog::get("logger");
    file_logger = spdlog::get("file_logger");
}

void FileDownloader::downloadCid(int cid_index) {
    fs::path filePath = fs::path(config["output"]) / cids[cid_index];
    std::ofstream outfile(filePath, std::ios::binary);
    if (!outfile.is_open()) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to open file: {}",
                            filePath.string());
        cidDownloadStatus[cid_index] = DownloadStatus::Failed;
        return;
    }

    try {
        auto curl = createCurlHandle();

        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &outfile);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);

        long responseCode = 0;
        std::string url;
        const std::vector<std::string> &gateways = config["gateways"];
        const int timeout = config["timeout"];
        const int maxRetries = config["max_retries"];

        size_t count = 0;
        std::array<std::array<uint8_t, BLAKE2S_OUTBYTES>, 2> hashes = {};

        for (int retries = 0; retries < maxRetries; ++retries) {
            if (cids[cid_index].size() == 59) {
                url = fmt::format("https://{}.ipfs.nftstorage.link",
                                  cids[cid_index]);
                curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 2 * timeout);
            } else {
                auto randomIndex =
                    Random::uniqueInts(1, 0, gateways.size() - 1)[0];
                url = fmt::format("https://{}/{}", gateways[randomIndex],
                                  cids[cid_index]);
                curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);
            }
            curl_easy_setopt(curl.get(), CURLOPT_URL, url.data());
            SPDLOG_LOGGER_INFO(logger, "Downloading {} from {}",
                               cids[cid_index], url);

            if (curl_easy_perform(curl.get()) == CURLE_OK) {
                curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE,
                                  &responseCode);
                if (responseCode == 200) {
                    if (cids[cid_index].size() == 59) {
                        cidDownloadStatus[cid_index] =
                            DownloadStatus::Succeeded;
                        break;
                    } else {
                        count++;
                        size_t remainder = count % 2;
                        hashes[remainder] =
                            Utils::getBlake2Hash(filePath.string());
                        SPDLOG_LOGGER_INFO(logger, "Hashed {}",
                                           cids[cid_index]);
                        if (remainder == 0) {
                            if (Utils::compareHashes(hashes)) {
                                cidDownloadStatus[cid_index] =
                                    DownloadStatus::Succeeded;
                                break;
                            } else {
                                SPDLOG_LOGGER_ERROR(
                                    logger, "Error: Mismatched hashes of {}",
                                    cids[cid_index]);
                            }
                        }
                    }
                }
            }
            outfile.clear();
            outfile.seekp(0, std::ios::beg);
            SPDLOG_LOGGER_INFO(logger, "Redownload {} (attempt {})",
                               cids[cid_index], retries + 1);
            logger->flush();
        }

        if (responseCode != 200 || !Utils::compareHashes(hashes)) {
            SPDLOG_LOGGER_ERROR(
                logger, "Error: Download of {} failed after {} attempts",
                cids[cid_index], maxRetries);
            cidDownloadStatus[cid_index] = DownloadStatus::Failed;
        } else {
            SPDLOG_LOGGER_INFO(logger, "Finish {}", cids[cid_index]);
        }
    } catch (const std::exception &e) {
        SPDLOG_LOGGER_ERROR(logger, "Error: {}", e.what());
        cidDownloadStatus[cid_index] = DownloadStatus::Failed;
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
        SPDLOG_LOGGER_INFO(logger, "{:<{}}: {}", "Assemble", WIDTH + 2,
                           filename);
        SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}", "path", WIDTH, albumPath);
        SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}", "filename", WIDTH,
                           trackName);
        logger->flush();

        std::string output = config["output"].get<std::string>();
        fs::path filePath = fs::path(output) / fs::path(filename);

        if (cids.size() == 1) {
            // Move the single CID file to the output path
            fs::path cidPath = fs::path(output) / fs::path(cids.front());

            try {
                fs::rename(cidPath, filePath);
                SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {} -> {}", "info", WIDTH,
                                   cids.front(), filename);
            } catch (const fs::filesystem_error &e) {
                SPDLOG_LOGGER_ERROR(
                    logger, "Error: Failed to move file from {} to {}: {}",
                    cids.front(), filename, e.what());
                fileDownloadStatus = DownloadStatus::Failed;
                return;
            }
        } else {
            // Handle the case where there are multiple CID files
            std::ofstream outfile(filePath, std::ios::binary);
            if (!outfile.is_open()) {
                SPDLOG_LOGGER_ERROR(logger,
                                    "Error: Failed to open output file: {}",
                                    filePath.string());
                fileDownloadStatus = DownloadStatus::Failed;
                return;
            }

            std::vector<char> buffer(4096);
            for (const auto &cid : cids) {
                fs::path cidPath = fs::path(output) / fs::path(cid);

                std::ifstream infile(cidPath, std::ios::binary);
                if (!infile.is_open()) {
                    SPDLOG_LOGGER_ERROR(logger,
                                        "Error: Failed to open input file: {}",
                                        cidPath.string());
                    fileDownloadStatus = DownloadStatus::Failed;
                    return;
                }

                while (infile) {
                    infile.read(buffer.data(), buffer.size());
                    outfile.write(buffer.data(), infile.gcount());
                }
                infile.close();
                SPDLOG_LOGGER_INFO(file_logger, "{} -> {}", cid, filename);

                if (!fs::remove(cidPath)) {
                    SPDLOG_LOGGER_ERROR(logger,
                                        "Error: Failed to delete file: {}",
                                        cidPath.string());
                }
            }
            outfile.close();
            SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {} CIDs -> {}", "info", WIDTH,
                               cids.size(), filename);
        }
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

size_t FileDownloader::getNumCIDs() const {
    return cids.size();
}

Downloader::Downloader(const json &config, const Database &db)
    : config(config), db(db) {
    logger = spdlog::get("logger");
    file_logger = spdlog::get("file_logger");
    SPDLOG_LOGGER_INFO(file_logger, "start downloader");
    const int maxValue = db.countTracks();
    const int numFiles = config["num_files"];
    const int minValue = config["min_value"];
    fileDownloaders.reserve(numFiles);

    if (numFiles <= 0 || minValue < 0 || minValue >= maxValue) {
        SPDLOG_LOGGER_ERROR(
            logger,
            "Error: Invalid parameters for file download configuration");
        throw std::invalid_argument("");
    }

    std::vector<int> randomIndices =
        Random::uniqueInts(numFiles, minValue, maxValue);

    for (int index : randomIndices) {
        std::vector<std::string> cids = db.getTrackCIDs(index);
        std::string trackName = db.getTrackName(index);
        std::string albumPath = db.getAlbumPath(index);
        std::string extension = Utils::getExtension(trackName);
        std::string filename =
            fmt::format("{}.{}", Random::alphanumericString(20), extension);

        fileDownloaders.emplace_back(std::make_unique<FileDownloader>(
            filename, albumPath, trackName, extension, cids, config));
    }
}

size_t Downloader::getNumThreads() const {
    size_t numThreads = 0;
    for (auto &fileDownloader : fileDownloaders) {
        numThreads += fileDownloader->getNumCIDs();
    }
    if (numThreads <= config["num_files"]) {
        numThreads = config["num_files"];
    } else {
        numThreads = 4 * std::thread::hardware_concurrency();
    }
    return numThreads;
}

void Downloader::performDownloads() {
    fmt::print(stdout, "\n");
    dp::thread_pool threadPool(getNumThreads());
    for (auto &fileDownloader : fileDownloaders) {
        for (size_t i = 0; i < fileDownloader->getNumCIDs(); ++i) {
            auto downloadTask = [&fileDownloader, i]() {
                fileDownloader->downloadCid(i);
            };
            threadPool.enqueue_detach(downloadTask);
        }
    }
    threadPool.wait_for_tasks();
    SPDLOG_LOGGER_INFO(file_logger, "finish downloader");
}

void Downloader::assembleFiles() {
    SPDLOG_LOGGER_INFO(file_logger, "start assemble");
    for (auto &fileDownloader : fileDownloaders) {
        fileDownloader->assemble();
    }
    fmt::print(stdout, "\n");
    SPDLOG_LOGGER_INFO(file_logger, "finish assemble");
}

std::vector<FileInfo> Downloader::getFileInfo() const {
    std::vector<FileInfo> fileInfo;
    fileInfo.reserve(fileDownloaders.size());
    for (const auto &fileDownloader : fileDownloaders) {
        if (fileDownloader->getFileDownloadStatus() ==
            DownloadStatus::Succeeded) {
            fileInfo.push_back({fileDownloader->getFilename(),
                                fileDownloader->getExtension(),
                                fileDownloader->getAlbumPath(),
                                fileDownloader->getTrackName()});
        }
    }
    return fileInfo;
}
