#include "const.hpp"
#include "database.hpp"
#include "decoder.hpp"
#include "dir.hpp"
#include "downloader.hpp"
#include "fmtlog-inl.hpp"
#include "random.hpp"
#include <chrono>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <cstdio>

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
    std::string logPath = config["log"];

    FILE *log_file = fopen(logPath.data(), "w");
    if (!log_file) {
        fmt::print(stdout, "Failed to open log file: {}\n", logPath.data());
        return EXIT_FAILURE;
    }
    fmtlog::setLogFile(log_file, false);
    fmtlog::setHeaderPattern("{YmdHMSe} {l} {s:<16}: ");
    fmtlog::setLogLevel(fmtlog::DBG);

    std::chrono::high_resolution_clock::time_point start, end;

    logd("start while");
    while (true) {
        std::vector<FileInfo> fileInfos;
        auto start = std::chrono::high_resolution_clock::now();
        try {
            Database db(dbPath);
            Dir::deleteDirectory(outputDir);
            Dir::createDirectory(outputDir);

            Downloader downloader(config, db);

            start = std::chrono::high_resolution_clock::now();
            downloader.performDownloads();
            end = std::chrono::high_resolution_clock::now();
            auto duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                      start);
            double seconds = static_cast<double>(duration.count()) / 1000;
            logd("Downloads took {:.3f} second(s)", seconds);
            fmt::print(stdout, "Downloads took {:.3f} second(s)\n", seconds);

            downloader.assembleFiles();
            fileInfos = downloader.getFileInfo();
        } catch (const std::exception &e) {
            loge("{}", e.what());
            fmt::print(stdout, "Error: {}\n\n", e.what());
            fmtlog::poll();
            return EXIT_FAILURE;
        }
        fmtlog::poll();

        for (const auto &fileInfo : fileInfos) {
            try {
                fmt::print(stdout, "{:<{}}: {}\n", "PLAYING", WIDTH + 2,
                           fileInfo.filename);
                fmt::print(stdout, "  {:<{}}: {}\n", "path", WIDTH,
                           fileInfo.albumPath);
                fmt::print(stdout, "  {:<{}}: {}\n", "filename", WIDTH,
                           fileInfo.trackName);
                std::cout.flush();

                fs::path filePath = fs::path(outputDir) / fileInfo.filename;

                start = std::chrono::high_resolution_clock::now();
                // pipeName = "test.pcm";
                Decoder decoder(filePath.string(), fileInfo.extension,
                                pipeName);
                decoder.printMetadata();
                end = std::chrono::high_resolution_clock::now();
                auto duration =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        end - start);
                fmt::print(stdout, "  {:<{}}: {:.3f} ms\n", "took", WIDTH,
                           static_cast<double>(duration.count()) / 1000);

                decoder.decode();
            } catch (const std::exception &e) {
                loge("{}", e.what());
                fmt::print(stdout, "Error: {}\n\n", e.what());
                fmtlog::poll();
                continue;
            }
            fmtlog::poll();
        }
        logd("end-5z2ok9v4iik5tdykgms90qrc6");
        fmt::print(stdout, "end-5z2ok9v4iik5tdykgms90qrc6\n");
        fflush(log_file);
    }
    logd("finish while");
    fclose(log_file);

    return EXIT_SUCCESS;
}
