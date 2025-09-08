#include <fstream>
#include <iostream>
#include <string>

#include <fmt/format.h>

#include "downloader.hpp"
#include "track.hpp"

using json = nlohmann::json;

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <config> <query>\n";
        return 1;
    }

    try {
        std::ifstream cfg_stream(argv[1]);
        json config = json::parse(cfg_stream);
        cfg_stream.close();

        const std::string query = argv[2];

        const std::string url =
            fmt::format("https://{}.r2.cloudflarestorage.com/{}/{}",
                        config["r2"]["account_id"].get<std::string>(),
                        config["r2"]["bucket"].get<std::string>(),
                        config["r2"]["db_file"].get<std::string>());
        Track track = Track::load(url, config, query);
        auto track_info = track.get_json();
        std::cout << "Track JSON:\n" << track_info.dump(4) << "\n";

        Downloader dl(config, track_info);
        dl.download_file();

        if (dl.succeeded()) {
            std::cout << "dl.succeeded()" << "\n";
            auto filename = dl.assemble_file().value();
            std::cout << "filename: " << filename << "\n";
        } else {
            std::cout << "!dl.succeeded()" << "\n";
        }

        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
