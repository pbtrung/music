#ifndef DOWNLOADER_HPP
#define DOWNLOADER_HPP

#include "database.hpp"
#include <string>
#include <uv.h>
#include <vector>

#include "json.hpp"
using json = nlohmann::json;

enum download_status { DOWNLOAD_PENDING, DOWNLOAD_SUCCEEDED, DOWNLOAD_FAILED };

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
    void download(uv_loop_t *loop);
    void assemble();
    std::string get_filename() const;
    std::string get_ext() const;
    download_status get_file_download_status() const;
    std::string get_album_path() const;
    std::string get_track_name() const;

  private:
    static void on_cid_download_completed(uv_work_t *req, int status);
    static void download_cid(uv_work_t *req);

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
