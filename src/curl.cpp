#include "curl.hpp"

Curl::Curl() : curl_handle(curl_easy_init(), curl_deleter{}) {
    if (!curl_handle) {
        throw std::runtime_error("Failed to initialize CURL easy handle");
    }

    // Set write callback and userdata
    curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEFUNCTION,
                     &Curl::write_callback);
    curl_easy_setopt(curl_handle.get(), CURLOPT_WRITEDATA, this);
}

Curl::~Curl() = default;

size_t Curl::write_callback(char *ptr, size_t size, size_t nmemb,
                            void *userdata) {
    auto *self = static_cast<Curl *>(userdata);
    size_t total = size * nmemb;
    self->response_data.append(ptr, total);
    return total;
}

template <typename T> void Curl::set_option(CURLoption option, T value) {
    if constexpr (std::is_same_v<T, std::string> ||
                  std::is_same_v<T, const char *> ||
                  std::is_same_v<T, char *>) {
        if (curl_easy_setopt(curl_handle.get(), option, value) != CURLE_OK) {
            throw std::runtime_error("Failed to set string option");
        }
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        if (curl_easy_setopt(curl_handle.get(), option,
                             static_cast<long>(value)) != CURLE_OK) {
            throw std::runtime_error("Failed to set integer option");
        }
    } else {
        if (curl_easy_setopt(curl_handle.get(), option, value) != CURLE_OK) {
            throw std::runtime_error("Failed to set pointer option");
        }
    }
}

template <typename T> T Curl::get_info(CURLINFO info) const {
    T result{};
    if (curl_easy_getinfo(curl_handle.get(), info, &result) != CURLE_OK) {
        throw std::runtime_error("Failed to get CURL info");
    }
    return result;
}

void Curl::perform() {
    response_data.clear();
    if (curl_easy_perform(curl_handle.get()) != CURLE_OK) {
        throw std::runtime_error("Failed to perform CURL");
    }
}

// Explicit instantiations
template void Curl::set_option<long>(CURLoption, long);
template void Curl::set_option<std::string>(CURLoption, std::string);
template void Curl::set_option<const char *>(CURLoption, const char *);
template void Curl::set_option<void *>(CURLoption, void *);

template double Curl::get_info<double>(CURLINFO) const;
template long Curl::get_info<long>(CURLINFO) const;
template char *Curl::get_info<char *>(CURLINFO) const;
