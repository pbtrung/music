#pragma once

#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

class FileBlockReader {
  private:
    static constexpr size_t BLOCK_SIZE = 6'500'000;
    static constexpr size_t MIN_LAST_BLOCK = 1'000'000;

    std::ifstream file;
    size_t file_size;
    size_t current_pos;

  public:
    explicit FileBlockReader(const std::filesystem::path &filepath);

    // Calculate optimal block sizes
    std::vector<size_t> calculate_block_sizes() const;

    // Read next block, returns empty vector when done
    std::vector<std::byte> read_next_block();

    // Process all blocks with a callback
    template <typename Callback> void process_blocks(Callback &&callback);

    // Get file info
    size_t get_file_size() const {
        return file_size;
    }
    size_t get_current_position() const {
        return current_pos;
    }

    // Reset to beginning
    void reset();
};

// Template implementation must be in header
template <typename Callback>
void FileBlockReader::process_blocks(Callback &&callback) {
    auto block_sizes = calculate_block_sizes();

    file.seekg(0);
    current_pos = 0;

    for (size_t i = 0; i < block_sizes.size(); ++i) {
        std::vector<std::byte> buffer(block_sizes[i]);

        file.read(reinterpret_cast<char *>(buffer.data()), block_sizes[i]);
        size_t bytes_read = file.gcount();
        buffer.resize(bytes_read);

        // Call the callback with block index and data
        callback(i, std::span<const std::byte>{buffer});

        current_pos += bytes_read;
    }
}