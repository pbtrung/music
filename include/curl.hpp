#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <curl/curl.h>

class Curl {
  public:
    Curl();
    ~Curl();

    Curl(const Curl &) = delete;
    Curl &operator=(const Curl &) = delete;
    Curl(Curl &&) noexcept = default;
    Curl &operator=(Curl &&) noexcept = default;

    template <typename T> void set_option(CURLoption option, T value);

    template <typename T> T get_info(CURLINFO info) const;

    int perform();

    void set_header(std::string_view header);
    
    // New methods for better state management
    void clear_headers();
    void reset();

    const std::string &get_response() const noexcept {
        return response_data;
    }

    CURL *handle() const noexcept {
        return curl_handle.get();
    }

    static size_t file_write_callback(char *ptr, size_t size, size_t nmemb,
                                      void *userdata);
    void set_file_output(std::ofstream *file);
    void reset_string_output();

  private:
    struct curl_deleter {
        void operator()(CURL *ptr) const noexcept {
            curl_easy_cleanup(ptr);
        }
    };

    std::unique_ptr<CURL, curl_deleter> curl_handle;

    std::string response_data;
    struct curl_slist *headers = nullptr;

    static size_t write_callback(char *ptr, size_t size, size_t nmemb,
                                 void *userdata);
};
