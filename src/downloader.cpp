#include "downloader.hpp"
#include "const.hpp"
#include "random.hpp"
#include "utils.hpp"
#include <algorithm>
#include <fmt/core.h>
#include <fstream>
#include <iostream>

size_t headerCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    std::string header(static_cast<char *>(contents), size * nmemb);
    std::string *contentLengthStr = static_cast<std::string *>(userp);

    // Look for the Content-Length header
    if (header.find("Content-Length:") == 0) {
        size_t pos = header.find(':');
        if (pos != std::string::npos) {
            *contentLengthStr = header.substr(pos + 1);
            contentLengthStr->erase(
                0, contentLengthStr->find_first_not_of(" \t\r\n"));
        }
    }

    return size * nmemb;
}

size_t FileDownloader::writeCallback(void *ptr, size_t size, size_t nmemb,
                                     void *userdata) {
    std::ofstream *outfile = static_cast<std::ofstream *>(userdata);
    outfile->write(static_cast<const char *>(ptr), size * nmemb);
    return size * nmemb;
}

FileDownloader::FileDownloader(std::string_view filename,
                               std::string_view albumPath,
                               std::string_view trackName,
                               std::string_view extension,
                               const std::vector<std::string> &cids,
                               const nlohmann::json &config)
    : filename(filename), albumPath(albumPath), trackName(trackName),
      extension(extension), cids(cids), config(config) {
    cidDownloadStatus.resize(cids.size(), DownloadStatus::Pending);
}

std::future<void> FileDownloader::download() {
    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < cids.size(); ++i) {
        futures.push_back(downloadCid(cids[i], i));
    }

    // Return a future that completes when all downloads are done
    return std::async(std::launch::async,
                      [futures = std::move(futures)]() mutable {
                          for (auto &future : futures) {
                              future.wait();
                          }
                      });
}

std::future<void> FileDownloader::downloadCid(const std::string &cid,
                                              size_t index) {
    return std::async(std::launch::async,
                      [this, cid, index]() { performDownload(cid, index); });
}

void FileDownloader::performDownload(const std::string &cid, size_t index) {
    std::string url;
    fmt::print("Downloading {}\n", cid);
    std::cout.flush();

    fs::path filePath = fs::path(config["output"]) / cid;
    std::ofstream outfile(filePath, std::ios::binary);
    if (!outfile) {
        fmt::print(stderr, "Failed to open file {}\n", filePath.string());
        cidDownloadStatus[index] = DownloadStatus::Failed;
        return;
    }

    long responseCode = 0;
    std::vector<std::string> gateways = config["gateways"];
    int timeout = config["timeout"];
    int maxRetries = config["max_retries"];

    std::string contentLengthStr;
    CurlHandle curl;
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &outfile);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &contentLengthStr);

    for (int retries = 0; retries < maxRetries; ++retries) {
        if (cid.size() == 59) {
            url = fmt::format("https://{}.ipfs.nftstorage.link", cid);
        } else {
            std::string gateway =
                gateways[Random::uniqueInts(1, 0, gateways.size() - 1)[0]];
            url = fmt::format("https://{}/{}", gateway, cid);
        }

        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT,
                         (cid.size() == 59) ? 2 * timeout : timeout);

        CURLcode res = curl_easy_perform(curl.get());
        if (res != CURLE_OK) {
            fmt::print(stderr, "CURL error for {} (attempt {}): {}\n", cid,
                       retries + 1, curl_easy_strerror(res));
            outfile.seekp(0);
            continue;
        }

        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &responseCode);
        if (responseCode == 200) {
            // Convert contentLengthStr to a number
            std::stringstream ss(contentLengthStr);
            long contentLength = 0;
            ss >> contentLength;
            // Check the size of the downloaded file
            size_t fileSize = fs::file_size(filePath);
            if (fileSize == contentLength) {
                cidDownloadStatus[index] = DownloadStatus::Succeeded;
                break;
            } else {
                fmt::print(
                    stderr,
                    "File size mismatch for {}: Expected {}, but got {}\n", cid,
                    contentLength, fileSize);
                outfile.seekp(0);
                continue;
            }
        } else {
            fmt::print(stderr,
                       "Unsuccessful response for {} (attempt {}): {}\n", cid,
                       retries + 1, responseCode);
        }
    }

    if (responseCode != 200) {
        cidDownloadStatus[index] = DownloadStatus::Failed;
    }

    outfile.close();
}

void FileDownloader::assemble() {
    downloadStatus = areAllDownloadsSucceeded() ? DownloadStatus::Succeeded
                                                : DownloadStatus::Failed;

    if (downloadStatus == DownloadStatus::Succeeded) {
        fmt::print("\n");
        fmt::print("{:<{}} : {}\n", "Assemble", WIDTH + 2, filename);
        fmt::print("  {:<{}} : {}\n", "path", WIDTH, albumPath);
        fmt::print("  {:<{}} : {}\n", "filename", WIDTH, trackName);
        std::cout.flush();

        std::string outputDir = config["output"];
        fs::path filePath = fs::path(outputDir) / filename;
        std::ofstream outfile(filePath, std::ios::binary);
        if (!outfile) {
            throw std::runtime_error("Failed to open output file: " +
                                     filePath.string());
        }

        std::vector<char> buffer(4096);

        for (const auto &cid : cids) {
            fs::path cidPath = fs::path(outputDir) / cid;

            std::ifstream infile(cidPath, std::ios::binary);
            if (!infile) {
                throw std::runtime_error("Failed to open input file: " +
                                         cidPath.string());
            }

            while (infile.read(buffer.data(), buffer.size())) {
                outfile.write(buffer.data(), infile.gcount());
            }
            if (infile.bad()) {
                throw std::runtime_error("Error reading input file: " +
                                         cidPath.string());
            }
            infile.close();

            std::error_code ec;
            fs::remove(cidPath, ec);
            if (ec) {
                fmt::print(stderr, "Warning: Failed to delete file: {}",
                           cidPath.string());
            }
        }
        if (!outfile) {
            throw std::runtime_error("Error writing to output file: " +
                                     filePath.string());
        }
        outfile.close();
    }
}

bool FileDownloader::areAllDownloadsSucceeded() const {
    return std::all_of(cidDownloadStatus.begin(), cidDownloadStatus.end(),
                       [](DownloadStatus status) {
                           return status == DownloadStatus::Succeeded;
                       });
}

const std::string &FileDownloader::getFilename() const {
    return filename;
}

const std::string &FileDownloader::getExtension() const {
    return extension;
}

DownloadStatus FileDownloader::getDownloadStatus() const {
    return downloadStatus;
}

const std::string &FileDownloader::getAlbumPath() const {
    return albumPath;
}

const std::string &FileDownloader::getTrackName() const {
    return trackName;
}

Downloader::Downloader(const nlohmann::json &config, const Database &db)
    : config(config), db(db) {
    const int numFilesToDownload = config["num_files"];
    const int min_value = config["min_value"];
    const int num_tracks = db.countTracks();

    if (numFilesToDownload <= 0 || numFilesToDownload > num_tracks) {
        throw std::invalid_argument(
            "Must be between 1 and the total number of tracks.");
    }

    std::vector<int> randomTrackIds =
        Random::uniqueInts(numFilesToDownload, min_value, num_tracks);

    for (int trackId : randomTrackIds) {
        std::vector<std::string> cids = db.getTrackCIDs(trackId);
        std::string trackName = db.getTrackName(trackId);
        std::string albumPath = db.getAlbumPath(trackId);
        std::string extension = Utils::getExtension(trackName);
        if (extension.empty()) {
            extension = "Unknown";
        }

        std::string filename = Random::alphanumericString(20) + "." + extension;
        fileDownloaders.emplace_back(std::make_unique<FileDownloader>(
            filename, albumPath, trackName, extension, cids, config));
    }
}

void Downloader::performDownloads() {
    std::vector<std::future<void>> futures;
    fmt::print(stdout, "\n");
    for (const auto &downloader : fileDownloaders) {
        futures.push_back(downloader->download());
    }
    for (auto &future : futures) {
        future.wait();
    }
}

void Downloader::assembleFiles() {
    for (const auto &downloader : fileDownloaders) {
        downloader->assemble();
    }
    fmt::print(stdout, "\n");
}

std::vector<FileInfo> Downloader::getFileInfo() const {
    std::vector<FileInfo> infos;
    for (const auto &downloader : fileDownloaders) {
        if (downloader->getDownloadStatus() == DownloadStatus::Succeeded) {
            infos.push_back(
                {downloader->getFilename(), downloader->getExtension(),
                 downloader->getAlbumPath(), downloader->getTrackName()});
        }
    }
    return infos;
}