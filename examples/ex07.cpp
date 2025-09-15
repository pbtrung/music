#include <cassert>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "wirehair/wirehair.h"

int main(int argc, char *argv[]) {
    // Configuration
    constexpr int ORIGINAL_COUNT = 11;
    constexpr int RECOVERY_COUNT = 33;
    constexpr int REDUNDANT_COUNT = RECOVERY_COUNT - ORIGINAL_COUNT; // 22

    // Check command line arguments
    if (argc != 3) {
        std::cout << std::format("Usage: {} <input_file> <output_directory>\n",
                                 argv[0]);
        std::cout << std::format("Schema: {}/{} (original/total pieces)\n",
                                 ORIGINAL_COUNT, RECOVERY_COUNT);
        return 1;
    }

    std::string input_file = argv[1];
    std::string output_dir = argv[2];

    try {
        // Initialize Wirehair library
        if (wirehair_init() != Wirehair_Success) {
            std::cerr << "Failed to initialize Wirehair library\n";
            return 1;
        }

        std::cout << "Wirehair library initialized successfully\n";

        // Load input file
        std::ifstream file(input_file, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << std::format("Failed to open file: {}\n", input_file);
            return 1;
        }

        size_t file_size = file.tellg();
        file.seekg(0);

        std::vector<uint8_t> file_data(file_size);
        file.read(reinterpret_cast<char *>(file_data.data()), file_size);
        file.close();

        // Calculate block size (divide file into ORIGINAL_COUNT pieces)
        uint32_t block_size = (file_size + ORIGINAL_COUNT - 1) / ORIGINAL_COUNT;

        std::cout << std::format("Loaded file: {} ({} bytes)\n", input_file,
                                 file_size);
        std::cout << std::format("Block size: {} bytes\n", block_size);
        std::cout << std::format("Schema: {}/{} (original/total)\n",
                                 ORIGINAL_COUNT, RECOVERY_COUNT);
        std::cout << std::format("Redundant pieces: {}\n", REDUNDANT_COUNT);

        // Create encoder
        WirehairCodec encoder = wirehair_encoder_create(
            nullptr, file_data.data(), file_size, block_size);
        if (!encoder) {
            std::cerr << "Failed to create Wirehair encoder\n";
            return 1;
        }

        std::cout << "Encoder created successfully\n";

        // Create output directory
        std::filesystem::create_directories(output_dir);

        // Generate and save all pieces (original + redundant)
        std::vector<uint8_t> piece_data(block_size);
        int successful_pieces = 0;

        for (int piece_id = 0; piece_id < RECOVERY_COUNT; ++piece_id) {
            uint32_t bytes_out = block_size;

            // Generate piece
            WirehairResult result = wirehair_encode(
                encoder, piece_id, piece_data.data(), block_size, &bytes_out);

            if (result != Wirehair_Success) {
                std::cerr << std::format(
                    "Failed to encode piece {}: error {}\n", piece_id,
                    static_cast<int>(result));
                continue;
            }

            // Determine piece type and filename
            std::string piece_type =
                (piece_id < ORIGINAL_COUNT) ? "original" : "redundant";
            std::string filename = std::format(
                "{}/piece_{:02d}_{}.bin", output_dir, piece_id, piece_type);

            // Save piece to file
            std::ofstream piece_file(filename, std::ios::binary);
            if (!piece_file.is_open()) {
                std::cerr << std::format("Failed to create piece file: {}\n",
                                         filename);
                continue;
            }
            piece_file.write(reinterpret_cast<const char *>(piece_data.data()),
                             bytes_out);
            piece_file.close();

            successful_pieces++;

            if (piece_id < ORIGINAL_COUNT) {
                std::cout << std::format(
                    "Generated original piece {}: {} ({} bytes)\n", piece_id,
                    filename, bytes_out);
            } else {
                std::cout << std::format(
                    "Generated redundant piece {}: {} ({} bytes)\n", piece_id,
                    filename, bytes_out);
            }
        }

        // Create metadata file
        std::string metadata_file = std::format("{}/metadata.txt", output_dir);
        std::ofstream meta(metadata_file);
        if (meta.is_open()) {
            meta << std::format("Original file: {}\n", input_file);
            meta << std::format("File size: {} bytes\n", file_size);
            meta << std::format("Block size: {} bytes\n", block_size);
            meta << std::format("Original pieces: {}\n", ORIGINAL_COUNT);
            meta << std::format("Total pieces: {}\n", RECOVERY_COUNT);
            meta << std::format("Redundant pieces: {}\n", REDUNDANT_COUNT);
            meta << std::format("Generated pieces: {}\n", successful_pieces);
            meta.close();

            std::cout << std::format("Metadata saved to: {}\n", metadata_file);
        }

        // Clean up
        wirehair_free(encoder);

        std::cout << std::format("\nEncoding complete!\n");
        std::cout << std::format("Generated {} out of {} pieces\n",
                                 successful_pieces, RECOVERY_COUNT);
        std::cout << std::format("Original pieces: 0-{}\n", ORIGINAL_COUNT - 1);
        std::cout << std::format("Redundant pieces: {}-{}\n", ORIGINAL_COUNT,
                                 RECOVERY_COUNT - 1);
        std::cout << std::format("Output directory: {}\n", output_dir);

        // Verification info
        std::cout << std::format("\nRecovery capability:\n");
        std::cout << std::format(
            "- Can recover original file from any {} pieces\n", ORIGINAL_COUNT);
        std::cout << std::format(
            "- Can lose up to {} pieces and still recover\n", REDUNDANT_COUNT);

        return 0;

    } catch (const std::exception &e) {
        std::cerr << std::format("Exception: {}\n", e.what());
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception occurred\n";
        return 1;
    }
}