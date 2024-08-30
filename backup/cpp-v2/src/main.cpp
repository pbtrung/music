#include "const.hpp"
#include "database.hpp"
#include "decoder.hpp"
#include "dir.hpp"
#include "downloader.hpp"
#include "random.hpp"
#include <chrono>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include "spdlog/sinks/stdout_color_sinks.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "json.hpp"
using json = nlohmann::json;

#include <filesystem>
namespace fs = std::filesystem;

json readConfig(std::string_view configFilename) {
    std::ifstream file(configFilename.data());
    if (!file) {
        fmt::print(stdout, "Failed to open file: {}\n", configFilename);
        std::exit(EXIT_FAILURE);
    }

    json config;
    file >> config;
    return config;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fmt::print(stdout, "Usage: {} {}\n", argv[0], "<config_file>");
        return EXIT_FAILURE;
    }

    std::string_view configFilename = argv[1];
    json config = readConfig(configFilename);

    std::string outputDir = config["output"];
    std::string pipeName = config["pipe_name"];
    std::string dbPath = config["db"];
    std::string logPath = config["log"];

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);
    console_sink->set_pattern("%v");

    auto file_sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true);
    file_sink->set_level(spdlog::level::info);
    file_sink->set_pattern("%d/%m/%Y %T.%e %l %s:%#: %v");
    auto file_logger =
        std::make_shared<spdlog::logger>("file_logger", file_sink);

    spdlog::sinks_init_list sink_list = {console_sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>("logger", begin(sink_list),
                                                   end(sink_list));
    logger->flush_on(spdlog::level::err);
    spdlog::register_logger(file_logger);
    spdlog::register_logger(logger);

    std::chrono::high_resolution_clock::time_point start, end;

    SPDLOG_LOGGER_INFO(file_logger, "start main loop");
    while (true) {
        std::vector<FileInfo> fileInfos;
        auto start = std::chrono::high_resolution_clock::now();
        try {
            Database db(dbPath);
            Dir::deleteDirectory(outputDir);
            Dir::createDirectory(outputDir);

            start = std::chrono::high_resolution_clock::now();
            Downloader downloader(config, db);
            end = std::chrono::high_resolution_clock::now();
            auto dur_usec =
                std::chrono::duration_cast<std::chrono::microseconds>(end -
                                                                      start);
            double mseconds = static_cast<double>(dur_usec.count()) / 1000;

            start = std::chrono::high_resolution_clock::now();
            downloader.performDownloads();
            end = std::chrono::high_resolution_clock::now();
            auto dur_msec =
                std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                      start);
            double seconds = static_cast<double>(dur_msec.count()) / 1000;
            SPDLOG_LOGGER_INFO(logger, "Initialization took {:.3f} ms",
                               mseconds);
            SPDLOG_LOGGER_INFO(logger, "Downloads took {:.3f} second(s)",
                               seconds);
            logger->flush();

            downloader.assembleFiles();
            fileInfos = downloader.getFileInfo();
        } catch (const std::exception &e) {
            fmt::print(stdout, "\n");
            logger->flush();
            return EXIT_FAILURE;
        }
        logger->flush();

        for (const auto &fileInfo : fileInfos) {
            try {
                SPDLOG_LOGGER_INFO(logger, "{:<{}}: {}", "PLAYING", WIDTH + 2,
                                   fileInfo.filename);
                SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}", "path", WIDTH,
                                   fileInfo.albumPath);
                SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}", "filename", WIDTH,
                                   fileInfo.trackName);
                logger->flush();

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
                SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {:.3f} ms", "took", WIDTH,
                                   static_cast<double>(duration.count()) /
                                       1000);
                logger->flush();

                decoder.decode();
            } catch (const std::exception &e) {
                fmt::print(stdout, "\n");
                logger->flush();
                continue;
            }
            logger->flush();
        }
        SPDLOG_LOGGER_INFO(logger, "end-5z2ok9v4iik5tdykgms90qrc6");
        logger->flush();
    }
    SPDLOG_LOGGER_INFO(file_logger, "finish main loop");

    return EXIT_SUCCESS;
}
