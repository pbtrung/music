#include "downloader.hpp"
#include "const.hpp"
#include "random.hpp"
#include "utils.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <iostream>

size_t file_downloader::write_cb(void *ptr, size_t size, size_t nmemb,
                                 void *userdata) {
    std::ofstream *outfile = static_cast<std::ofstream *>(userdata);
    outfile->write(static_cast<const char *>(ptr), size * nmemb);
    return size * nmemb;
}

file_downloader::file_downloader(const std::string &filename,
                                 const std::string &album_path,
                                 const std::string &track_name,
                                 const std::string &ext,
                                 const std::vector<std::string> &cids,
                                 const json &config)
    : filename(filename), album_path(album_path), track_name(track_name),
      ext(ext), cids(cids), config(config) {
    cid_download_status.resize(cids.size(), download_status::PENDING);
    file_download_status = download_status::PENDING;
}

std::future<void> file_downloader::download() {
    std::vector<std::unique_ptr<std::future<void>>> futures;
    for (size_t i = 0; i < cids.size(); ++i) {
        auto future =
            std::make_unique<std::future<void>>(download_cid(cids[i], i));
        futures.push_back(std::move(future));
    }
    return std::async(std::launch::async,
                      [futures = std::move(futures)]() mutable {
                          for (auto &future : futures) {
                              future->get();
                          }
                      });
}

std::future<void> file_downloader::download_cid(const std::string &cid,
                                                size_t index) {
    return std::async(std::launch::async, [this, cid, index]() {
        fmt::print(stdout, "Downloading {}\n", cid);
        std::cout.flush();

        std::filesystem::path file_path =
            std::filesystem::path(config["output"].get<std::string>()) /
            std::filesystem::path(cid);

        std::ofstream outfile(file_path, std::ios::binary);
        if (!outfile.is_open()) {
            fmt::print(stderr, "Failed to open file {}\n", file_path.string());
            cid_download_status[index] = download_status::FAILED;
            return;
        }

        try {
            CurlHandle curl;

            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_cb);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &outfile);
            curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);

            long response_code = 0;
            std::string url;
            std::vector<std::string> gateways =
                config["gateways"].get<std::vector<std::string>>();
            int timeout = config["timeout"].get<int>();

            for (int retries = 0; retries < config["max_retries"].get<int>();
                 ++retries) {
                if (cid.size() == 59) {
                    url = fmt::format("https://{}.ipfs.nftstorage.link", cid);
                    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 2 * timeout);
                } else {
                    std::vector<int> random_indices =
                        rng::random_ints(1, 0, gateways.size() - 1);
                    url = fmt::format("https://{}/{}",
                                      gateways[random_indices[0]], cid);
                    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);
                }

                curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());

                CURLcode res = curl_easy_perform(curl.get());
                if (res == CURLE_OK) {
                    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE,
                                      &response_code);
                    if (response_code == 200) {
                        cid_download_status[index] = download_status::SUCCEEDED;
                        break;
                    }
                }
                outfile.clear();
                outfile.seekp(0, std::ios::beg);
            }

            if (response_code != 200) {
                fmt::print(stderr, "Download of cid {} failed\n", cid);
                cid_download_status[index] = download_status::FAILED;
            }
        } catch (const std::exception &e) {
            fmt::print(stderr, "Error: {}\n", e.what());
            cid_download_status[index] = download_status::FAILED;
        }
    });
}

void file_downloader::assemble() {
    file_download_status = download_status::SUCCEEDED;
    for (size_t i = 0; i < cids.size(); ++i) {
        if (cid_download_status[i] != download_status::SUCCEEDED) {
            file_download_status = download_status::FAILED;
        }
    }

    if (file_download_status == download_status::SUCCEEDED) {
        fmt::print(stdout, "\n");
        fmt::print(stdout, "{:<{}}: {}\n", "Assemble", WIDTH + 2, filename);
        fmt::print(stdout, "  {:<{}}: {}\n", "path", WIDTH, album_path);
        fmt::print(stdout, "  {:<{}}: {}\n", "filename", WIDTH, track_name);
        std::cout.flush();

        std::string output = config["output"].get<std::string>();
        std::filesystem::path file_path =
            std::filesystem::path(output) / std::filesystem::path(filename);
        std::ofstream outfile(file_path, std::ios::binary);
        if (!outfile.is_open()) {
            throw std::runtime_error("Failed to open output file: " +
                                     file_path.string());
        }

        std::vector<char> buffer(4096);

        for (const auto &cid : cids) {
            std::filesystem::path cid_path =
                std::filesystem::path(output) / std::filesystem::path(cid);

            std::ifstream infile(cid_path, std::ios::binary);
            if (!infile.is_open()) {
                throw std::runtime_error("Failed to open input file: " +
                                         cid_path.string());
            }

            while (infile) {
                infile.read(buffer.data(), buffer.size());
                outfile.write(buffer.data(), infile.gcount());
            }

            infile.close();

            if (!std::filesystem::remove(cid_path)) {
                throw std::runtime_error("Failed to delete file: " +
                                         cid_path.string());
            }
        }

        outfile.close();
    }
}

std::string file_downloader::get_filename() const { return filename; }
std::string file_downloader::get_ext() const { return ext; }
download_status file_downloader::get_file_download_status() const {
    return file_download_status;
}
std::string file_downloader::get_album_path() const { return album_path; }
std::string file_downloader::get_track_name() const { return track_name; }

downloader::downloader(const json &config, const database &db)
    : config(config), db(db) {
    int min_value = 1;
    int max_value = db.count_tracks();
    int num_files = config["num_files"].get<int>();
    std::vector<int> random_indices =
        rng::random_ints(num_files, min_value, max_value);

    for (int i = 0; i < num_files; ++i) {
        std::vector<std::string> cids = db.get_cids(random_indices[i]);
        std::string track_name = db.get_track_name(random_indices[i]);
        std::string album_path = db.get_album(random_indices[i]);
        std::string ext = utils::get_extension(track_name);
        std::string filename = rng::random_string(20) + "." + ext;

        file_downloaders.emplace_back(std::make_unique<file_downloader>(
            filename, album_path, track_name, ext, cids, config));
    }
}

void downloader::perform_downloads() {
    std::vector<std::future<void>> futures;
    fmt::print(stdout, "\n");
    for (auto &fder : file_downloaders) {
        futures.push_back(fder->download());
    }
    for (auto &future : futures) {
        future.get();
    }
}

void downloader::assemble_files() {
    for (auto &fder : file_downloaders) {
        fder->assemble();
    }
    fmt::print(stdout, "\n");
}

std::vector<file_info> downloader::get_file_info() const {
    std::vector<file_info> file_infos;
    for (auto &fder : file_downloaders) {
        file_infos.push_back({fder->get_filename(),
                              fder->get_file_download_status(), fder->get_ext(),
                              fder->get_album_path(), fder->get_track_name()});
    }
    return file_infos;
}
