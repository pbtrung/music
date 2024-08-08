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
#include <string_view>

#include "json.hpp"
using json = nlohmann::json;

#include <filesystem>
namespace fs = std::filesystem;

json readConfig(std::string_view configFilename) {
    std::ifstream file(configFilename.data());
    if (!file) {
        fmt::print(stderr, "Could not open: {}\n", configFilename);
        std::exit(EXIT_FAILURE);
    }

    json config;
    file >> config;
    return config;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fmt::print(stderr, "Usage: {} {}\n", argv[0], "<config_file>");
        return EXIT_FAILURE;
    }

    std::string_view configFilename = argv[1];
    json config = readConfig(configFilename);

    std::string outputDir = config["output"];
    std::string pipeName = config["pipe_name"];
    std::string dbPath = config["db"];

    while (true) {
        std::vector<FileInfo> fileInfos;
        try {
            Database db(dbPath);
            Dir::deleteDirectory(outputDir);
            Dir::createDirectory(outputDir);

            Downloader downloader(config, db);
            downloader.performDownloads();
            downloader.assembleFiles();

            fileInfos = downloader.getFileInfo();
        } catch (const std::exception &e) {
            fmt::print(stderr, "Error: {}\n\n", e.what());
            return EXIT_FAILURE;
        }

        for (const auto &fileInfo : fileInfos) {
            try {
                fmt::print(stdout,
                           "{:<{}} : {}\n",
                           "PLAYING",
                           WIDTH + 2,
                           fileInfo.filename);
                fmt::print(stdout,
                           "  {:<{}} : {}\n",
                           "path",
                           WIDTH,
                           fileInfo.albumPath);
                fmt::print(stdout,
                           "  {:<{}} : {}\n",
                           "filename",
                           WIDTH,
                           fileInfo.trackName);
                std::cout.flush();

                fs::path filePath = fs::path(outputDir) / fileInfo.filename;
                // pipeName = "test.pcm";
                Decoder decoder(filePath, pipeName);
                decoder.printMetadata();
                decoder.decode();
            } catch (const std::exception &e) {
                fmt::print(stderr, "Error: {}\n\n", e.what());
                continue;
            }
        }
        fmt::print(stdout, "end-5z2ok9v4iik5tdykgms90qrc6\n");
    }
    return EXIT_SUCCESS;
}
