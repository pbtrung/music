#ifndef DOWNLOADER_HPP
#define DOWNLOADER_HPP

#include "database.hpp"
#include <coroutine>
#include <curl/curl.h>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"
using json = nlohmann::json;

enum class download_status { PENDING, SUCCEEDED, FAILED };

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

    CURL *get() const { return handle; }

  private:
    CURL *handle;
};

struct file_info {
    std::string filename;
    download_status file_download_status;
    std::string ext;
    std::string album_path;
    std::string track_name;
};

class file_downloader {
  public:
    file_downloader(const std::string &filename, const std::string &album_path,
                    const std::string &track_name, const std::string &ext,
                    const std::vector<std::string> &cids, const json &config);

    std::future<void> download();
    void assemble();
    std::string get_filename() const;
    std::string get_ext() const;
    download_status get_file_download_status() const;
    std::string get_album_path() const;
    std::string get_track_name() const;

  private:
    static size_t write_cb(void *ptr, size_t size, size_t nmemb,
                           void *userdata);
    std::future<void> download_cid(const std::string &cid, size_t index);

    std::string filename;
    std::string album_path;
    std::string track_name;
    std::string ext;
    std::vector<std::string> cids;
    json config;
    std::vector<download_status> cid_download_status;
    download_status file_download_status;
};

class downloader {
  public:
    downloader(const json &config, const database &db);
    void perform_downloads();
    void assemble_files();
    std::vector<file_info> get_file_info() const;

  private:
    json config;
    database db;
    std::vector<std::unique_ptr<file_downloader>> file_downloaders;
};

#endif // DOWNLOADER_HPP
