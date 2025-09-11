#include <stdexcept>

#include "file_block_reader.hpp"

FileBlockReader::FileBlockReader(const std::filesystem::path &filepath)
    : file(filepath, std::ios::binary), current_pos(0) {

    if (!file) {
        throw std::runtime_error("Cannot open file: " + filepath.string());
    }

    // Get file size
    file_size = std::filesystem::file_size(filepath);
}

std::vector<size_t> FileBlockReader::calculate_block_sizes() const {
    std::vector<size_t> block_sizes;

    if (file_size == 0) {
        return block_sizes; // Empty file
    }

    if (file_size <= BLOCK_SIZE) {
        // File fits in one block
        block_sizes.push_back(file_size);
        return block_sizes;
    }

    // Calculate how many full blocks we can have
    size_t num_full_blocks = file_size / BLOCK_SIZE;
    size_t remainder = file_size % BLOCK_SIZE;

    if (remainder == 0) {
        // Perfect fit with full blocks
        for (size_t i = 0; i < num_full_blocks; ++i) {
            block_sizes.push_back(BLOCK_SIZE);
        }
    } else if (remainder >= MIN_LAST_BLOCK) {
        // Remainder is large enough to be its own block
        for (size_t i = 0; i < num_full_blocks; ++i) {
            block_sizes.push_back(BLOCK_SIZE);
        }
        block_sizes.push_back(remainder);
    } else {
        // Remainder is too small, need to merge and split
        if (num_full_blocks > 0) {
            // Add all but the last full block
            for (size_t i = 0; i < num_full_blocks - 1; ++i) {
                block_sizes.push_back(BLOCK_SIZE);
            }

            // The merged block is the last full block + remainder
            size_t merged_block_size = BLOCK_SIZE + remainder;

            // Split the merged block in half
            size_t first_half = merged_block_size / 2;
            size_t second_half = merged_block_size - first_half;

            block_sizes.push_back(first_half);
            block_sizes.push_back(second_half);
        } else {
            // Only one "block" that's larger than BLOCK_SIZE, split it
            size_t first_half = file_size / 2;
            size_t second_half = file_size - first_half;
            block_sizes.push_back(first_half);
            block_sizes.push_back(second_half);
        }
    }

    return block_sizes;
}

std::vector<std::byte> FileBlockReader::read_next_block() {
    if (current_pos >= file_size) {
        return {}; // EOF
    }

    auto block_sizes = calculate_block_sizes();

    // Find which block we're currently reading
    size_t block_index = 0;
    size_t pos_in_blocks = 0;

    for (size_t i = 0; i < block_sizes.size(); ++i) {
        if (current_pos >= pos_in_blocks &&
            current_pos < pos_in_blocks + block_sizes[i]) {
            block_index = i;
            break;
        }
        pos_in_blocks += block_sizes[i];
    }

    if (block_index >= block_sizes.size()) {
        return {}; // EOF
    }

    size_t bytes_to_read =
        block_sizes[block_index] - (current_pos - pos_in_blocks);

    std::vector<std::byte> buffer(bytes_to_read);

    file.seekg(current_pos);
    file.read(reinterpret_cast<char *>(buffer.data()), bytes_to_read);

    size_t bytes_read = file.gcount();
    buffer.resize(bytes_read);

    current_pos += bytes_read;

    return buffer;
}

void FileBlockReader::reset() {
    file.seekg(0);
    current_pos = 0;
}