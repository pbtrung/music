#include <fstream>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

#include "curl.hpp"

Curl::Curl() : curl_handle(curl_easy_init(), curl_deleter{}) {
    if (!curl_handle) {
        throw std::runtime_error("Failed to initialize CURL easy handle");
    }

    // Set default write callback for string response
    curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEFUNCTION,
                     &Curl::write_callback);
    curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEDATA, this);
}

Curl::~Curl() {
    if (headers) {
        curl_slist_free_all(headers);
    }
}

size_t Curl::write_callback(char *ptr, size_t size, size_t nmemb,
                            void *userdata) {
    auto *self = static_cast<Curl *>(userdata);
    size_t total = size * nmemb;
    self->response_data.append(ptr, total);
    return total;
}

// New static callback for file writing
size_t Curl::file_write_callback(char *ptr, size_t size, size_t nmemb,
                                 void *userdata) {
    auto *file = static_cast<std::ofstream *>(userdata);
    size_t total = size * nmemb;
    file->write(ptr, total);
    return file->good() ? total : 0;
}

template <typename T> void Curl::set_option(CURLoption option, T value) {
    CURLcode result = CURLE_OK;

    if constexpr (std::is_same_v<T, std::string>) {
        result = curl_easy_setopt(curl_handle.get(), option, value.c_str());
    } else if constexpr (std::is_same_v<T, const char *> ||
                         std::is_same_v<T, char *>) {
        result = curl_easy_setopt(curl_handle.get(), option, value);
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        result = curl_easy_setopt(curl_handle.get(), option,
                                  static_cast<long>(value));
    } else {
        result = curl_easy_setopt(curl_handle.get(), option, value);
    }

    if (result != CURLE_OK) {
        SPDLOG_TRACE("Failed to set CURL option {}: {}",
                     static_cast<int>(option), curl_easy_strerror(result));
        throw std::runtime_error("Failed to set CURL option: " +
                                 std::string(curl_easy_strerror(result)));
    }
}

template <typename T> T Curl::get_info(CURLINFO info) const {
    T result{};
    if (curl_easy_getinfo(curl_handle.get(), info, &result) != CURLE_OK) {
        throw std::runtime_error("Failed to get CURL info");
    }
    return result;
}

int Curl::perform() {
    response_data.clear();
    return curl_easy_perform(curl_handle.get());
}

void Curl::set_header(std::string_view header) {
    headers = curl_slist_append(headers, std::string(header).c_str());
    if (!headers) {
        throw std::runtime_error("Failed to append header");
    }
    curl_easy_setopt(curl_handle.get(), CURLOPT_HTTPHEADER, headers);
}

// Method to configure for file writing
void Curl::set_file_output(std::ofstream *file) {
    curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEFUNCTION,
                     &Curl::file_write_callback);
    curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEDATA, file);
}

// Method to reset to string output
void Curl::reset_string_output() {
    curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEFUNCTION,
                     &Curl::write_callback);
    curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEDATA, this);
}

void Curl::clear_headers() {
    if (headers) {
        curl_slist_free_all(headers);
        headers = nullptr;
    }
    curl_easy_setopt(curl_handle.get(), CURLOPT_HTTPHEADER, nullptr);
}

void Curl::reset() {
    response_data.clear();
    clear_headers();
    // Reset to default string output callbacks
    reset_string_output();
}

// Explicit instantiations
template void Curl::set_option<long>(CURLoption, long);
template void Curl::set_option<int>(CURLoption, int);
template void Curl::set_option<std::string>(CURLoption, std::string);
template void Curl::set_option<const char *>(CURLoption, const char *);
template void Curl::set_option<char *>(CURLoption, char *);
template void Curl::set_option<void *>(CURLoption, void *);

template double Curl::get_info<double>(CURLINFO) const;
template long Curl::get_info<long>(CURLINFO) const;
template char *Curl::get_info<char *>(CURLINFO) const;
