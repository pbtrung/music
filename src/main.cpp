#include "const.hpp"
#include "database.hpp"
#include "decoder.hpp"
#include "dir.hpp"
#include "downloader.hpp"
#include "random.hpp"
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <string>

#include "json.hpp"
using json = nlohmann::json;

json read_config(std::string config_filename) {
    std::ifstream file(config_filename);
    if (!file.is_open()) {
        fmt::print(stderr, "Could not open: {}\n", config_filename);
        exit(-1);
    }
    json config;
    file >> config;
    file.close();

    return config;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fmt::print(stderr, "Usage: {} {}\n", argv[0], "<config_file>");
        return -1;
    }
    std::string config_filename = argv[1];
    json config = read_config(config_filename);
    std::string output = config["output"].get<std::string>();
    std::string pipe_name = config["pipe_name"].get<std::string>();
    std::string db_path = config["db"].get<std::string>();

    while (true) {
        std::vector<file_info> file_infos;

        try {
            database db(db_path);

            dir::delete_directory(output);
            dir::create_directory(output);

            downloader dler(config, db);
            dler.perform_downloads();
            dler.assemble_files();
            file_infos = dler.get_file_info();
        } catch (const std::exception &e) {
            fmt::print(stderr, "Error: {}\n", e.what());
            return -1;
        }

        for (int i = 0; i < file_infos.size(); ++i) {
            try {
                if (file_infos[i].file_download_status ==
                    download_status::SUCCEEDED) {
                    fmt::print(stdout, "{:<{}}: {}\n", "PLAYING", WIDTH + 2,
                               file_infos[i].filename);
                    fmt::print(stdout, "  {:<{}}: {}\n", "path", WIDTH,
                               file_infos[i].album_path);
                    fmt::print(stdout, "  {:<{}}: {}\n", "filename", WIDTH,
                               file_infos[i].track_name);
                    std::cout.flush();

                    std::filesystem::path file_path =
                        std::filesystem::path(output) /
                        std::filesystem::path(file_infos[i].filename);

                    // pipe_name = "test.pcm";
                    decoder decoder(file_path.string(), file_infos[i].ext,
                                    pipe_name);
                    decoder.decode();
                }
            } catch (const std::exception &e) {
                fmt::print(stderr, "Error: {}\n", e.what());
                continue;
            }
        }

        fmt::print(stdout, "end-5z2ok9v4iik5tdykgms90qrc6\n");
    }

    return 0;
}
