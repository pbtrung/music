#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "curl.hpp"
#include "json.hpp"

enum class DownloadStatus { PENDING, SUCCEEDED, FAILED };
enum class CidType { ARW, IPFS, GDR };

// Base class for all downloaders
class BaseDownloader {
  public:
    explicit BaseDownloader(nlohmann::json &config,
                            const nlohmann::json &track);
    virtual ~BaseDownloader() = default;

    BaseDownloader(const BaseDownloader &) = delete;
    BaseDownloader &operator=(const BaseDownloader &) = delete;
    BaseDownloader(BaseDownloader &&) = default;
    BaseDownloader &operator=(BaseDownloader &&) = default;

    virtual bool download(const std::string &cid, std::ofstream &outfile) = 0;

  protected:
    // Utility methods available to all derived classes
    void reset_file_position(std::ofstream &outfile) const;
    std::string cid_type_to_string(CidType type) const;

    // Time utilities
    static std::string
    to_iso8601(const std::chrono::system_clock::time_point &tp);
    static std::chrono::system_clock::time_point
    from_iso8601(const std::string &iso_string);

    const nlohmann::json &track;
    nlohmann::json &config;
};

// IPFS downloader for special CIDs
class IPFSDownloader : public BaseDownloader {
  public:
    explicit IPFSDownloader(nlohmann::json &config,
                            const nlohmann::json &track);

    bool download(const std::string &cid, std::ofstream &outfile) override;

  private:
    bool try_ipfs_download(const std::string &cid, std::ofstream &outfile,
                           const std::string &url, int timeout);
    std::string build_ipfs_url(const std::string &cid, int attempt) const;
    std::string get_gateway(int attempt) const;
    bool validate_ipfs_response(const Curl &curl) const;
};

// ARWeave downloader for standard IPFS gateways
class ARWDownloader : public BaseDownloader {
  public:
    explicit ARWDownloader(nlohmann::json &config, const nlohmann::json &track);

    bool download(const std::string &cid, std::ofstream &outfile) override;

  private:
    bool try_arw_download(const std::string &cid, std::ofstream &outfile,
                          const std::string &url, int timeout);
    std::string build_arw_url(const std::string &cid, int attempt) const;
    std::string get_gateway(int attempt) const;
    bool validate_arw_response(const Curl &curl) const;
};

// Google Drive downloader
class GDRDownloader : public BaseDownloader {
  public:
    explicit GDRDownloader(nlohmann::json &config, const nlohmann::json &track);

    bool download(const std::string &cid, std::ofstream &outfile) override;

  private:
    std::string get_fresh_token();
    std::string request_new_token();
    bool is_token_valid() const;
    int get_account_index_by_email(const std::string &email) const;
};

// Main downloader orchestrator
class Downloader {
  public:
    explicit Downloader(const nlohmann::json &config,
                        const nlohmann::json &track);
    ~Downloader() = default;

    Downloader(const Downloader &) = delete;
    Downloader &operator=(const Downloader &) = delete;
    Downloader(Downloader &&) = default;
    Downloader &operator=(Downloader &&) = default;

    void download_file();
    bool succeeded() const;
    std::optional<std::string> assemble_file();

  private:
    // Main download workflow
    void download_single_cid(int cid_index);
    bool execute_download(const std::string &cid, std::ofstream &outfile);
    void finalize_download(int cid_index, const std::string &cid,
                           const std::filesystem::path &temp_path,
                           bool success);

    // CID type detection and downloader creation
    CidType get_cid_type(const std::string &cid) const;
    std::unique_ptr<BaseDownloader> create_downloader(CidType type);

    // File management
    std::filesystem::path get_temp_path(const std::string &cid) const;
    std::filesystem::path get_final_path(const std::string &cid,
                                         CidType type) const;
    void cleanup_temp_file(const std::filesystem::path &temp_path) const;

    // Assembly methods
    std::optional<std::string> assemble_multiple_files();
    std::optional<std::string> handle_single_file();
    bool combine_cid_files(const std::filesystem::path &output_path);
    void cleanup_cid_files();

    // Utilities
    std::string generate_output_filename() const;
    void log_download_progress(int cid_index, const std::string &cid);
    void ensure_output_directory() const;
    std::string cid_type_to_string(CidType type) const;

    nlohmann::json track;
    nlohmann::json config;
    std::vector<DownloadStatus> cid_download_status;
    std::atomic<int> completed_cids;

    // Cached downloaders for each type
    mutable std::unique_ptr<IPFSDownloader> ipfs_downloader;
    mutable std::unique_ptr<ARWDownloader> arw_downloader;
    mutable std::unique_ptr<GDRDownloader> gdr_downloader;
};
