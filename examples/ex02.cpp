#include <iostream>
#include <string>

#include "track.hpp"

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <url> <query>\n";
        return 1;
    }

    try {
        const std::string url = argv[1];
        const std::string query = argv[2];

        Track track = Track::load(url, query);

        std::cout << "Track ID: " << track.get_id() << "\n";
        std::cout << "Track JSON:\n" << track.get_json().dump(4) << "\n";

        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
