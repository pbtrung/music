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

typedef struct {
    std::string cid;
    std::string filename;
    download_status *cid_download_status;
    json config;
} download_info_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
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
    cid_download_status.resize(cids.size(), DOWNLOAD_PENDING);
    file_download_status = DOWNLOAD_PENDING;
}

void file_downloader::download_cid(uv_work_t *req) {
    download_info_t *download_info = static_cast<download_info_t *>(req->data);
    fmt::print(stdout, "Downloading {}\n", download_info->cid);
    std::cout.flush();

    std::filesystem::path file_path =
        std::filesystem::path(
            download_info->config["output"].get<std::string>()) /
        std::filesystem::path(download_info->cid);

    std::ofstream outfile(file_path, std::ios::binary);
    if (!outfile.is_open()) {
        fmt::print(stderr, "Failed to open file {}\n", file_path.string());
        *(download_info->cid_download_status) = DOWNLOAD_FAILED;
        return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fmt::print(stderr, "Error initializing curl handle\n");
        outfile.close();
        *(download_info->cid_download_status) = DOWNLOAD_FAILED;
        return;
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outfile);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    long response_code = 0;
    std::string url;
    std::vector<std::string> gateways =
        download_info->config["gateways"].get<std::vector<std::string>>();
    int timeout = download_info->config["timeout"].get<int>();

    for (int retries = 0;
         retries < download_info->config["max_retries"].get<int>(); ++retries) {
        if (download_info->cid.size() == 59) {
            url = fmt::format("https://{}.ipfs.nftstorage.link",
                              download_info->cid);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2 * timeout);
        } else {
            std::vector<int> random_indices =
                rng::random_ints(1, 0, gateways.size() - 1);
            url = fmt::format("https://{}/{}", gateways[random_indices[0]],
                              download_info->cid);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            if (response_code == 200) {
                *(download_info->cid_download_status) = DOWNLOAD_SUCCEEDED;
                break;
            }
        }
        outfile.clear();
        outfile.seekp(0, std::ios::beg);
    }

    if (response_code != 200) {
        fmt::print(stderr, "Download of cid {} failed\n", download_info->cid);
        *(download_info->cid_download_status) = DOWNLOAD_FAILED;
    }

    outfile.close();
    curl_easy_cleanup(curl);
}

void file_downloader::on_cid_download_completed(uv_work_t *req, int status) {
    download_info_t *download_info = (download_info_t *)req->data;
    delete download_info;
    delete req;
}

void file_downloader::download(uv_loop_t *loop) {
    for (int i = 0; i < cids.size(); ++i) {
        uv_work_t *req = new uv_work_t;
        download_info_t *download_info = new download_info_t;

        download_info->cid = cids[i];
        download_info->filename = filename;
        download_info->cid_download_status = &cid_download_status[i];
        download_info->config = config;
        req->data = download_info;

        uv_queue_work(loop, req, download_cid, on_cid_download_completed);
    }
}

void file_downloader::assemble() {
    file_download_status = DOWNLOAD_SUCCEEDED;
    for (int i = 0; i < cids.size(); ++i) {
        if (cid_download_status[i] != DOWNLOAD_SUCCEEDED) {
            file_download_status = DOWNLOAD_FAILED;
        }
    }

    if (file_download_status == DOWNLOAD_SUCCEEDED) {
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

        for (int j = 0; j < cids.size(); ++j) {
            std::filesystem::path cid_path =
                std::filesystem::path(output) / std::filesystem::path(cids[j]);

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
    uv_loop_t *loop = uv_default_loop();
    fmt::print(stdout, "\n");
    for (auto &fder : file_downloaders) {
        fder->download(loop);
    }
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
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
