#include <iostream>

#include "curl.hpp"

int main() {
    try {
        Curl curl;
        curl.set_option(CURLOPT_URL, "https://google.com");
        curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);

        curl.perform();

        long status = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
        std::cout << "HTTP status: " << status << "\n";
        std::cout << "Response:\n" << curl.get_response() << "\n";
    } catch (const std::exception &e) {
        std::cerr << "Curl error: " << e.what() << "\n";
    }
}
