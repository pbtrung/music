# Block Storage + Index Design

## Input
- A list of buffers (`Vec<u8>`).

## Packing
- Concatenate buffers into **1 MiB uncompressed blocks**.
- Split buffers across blocks if needed.
- Buffers larger than 1 MiB are split automatically.

## Compression
- Compress each block with **zstd**.

## Output
- **Data file**: concatenated compressed blocks.
- **Index file**: binary, little-endian.

## Index Format

### Header
- **Number of compressed blocks (u64)**
- For each block:
  - **Compressed size (u64)** → enables seeking to any block.

### Buffer Segments
- **Buffer index starts at 1**.
- For each buffer:
  - **Number of segments (u64)**
  - For each segment:
    - **Block index (u64)** → identifies the compressed block.
    - **Uncompressed offset (u64)** → position inside the block.
    - **Length (u64)** → number of bytes from this block.

## Recovery
1. Load the index.
2. Use compressed block sizes to calculate file positions.
3. For a given buffer, look up its segments.
4. Seek and decompress only the necessary compressed block(s).
5. Slice and concatenate to reconstruct the buffer.
6. Random access supported.

## Index File Binary Layout (Little-Endian)

```
┌─────────────────────────────────────────────────────────────┐
│                        HEADER                               │
├─────────────────────────────────────────────────────────────┤
│ num_blocks: u64                                             │
├─────────────────────────────────────────────────────────────┤
│ block_0_compressed_size: u64                                │
├─────────────────────────────────────────────────────────────┤
│ block_1_compressed_size: u64                                │
├─────────────────────────────────────────────────────────────┤
│ ...                                                         │
├─────────────────────────────────────────────────────────────┤
│ block_(num_blocks-1)_compressed_size: u64                   │
├─────────────────────────────────────────────────────────────┤
│                   BUFFER SEGMENTS                           │
├─────────────────────────────────────────────────────────────┤
│ buffer_1_num_segments: u64                                  │
├─────────────────────────────────────────────────────────────┤
│   segment_0_block_index: u64                                │
│   segment_0_uncompressed_offset: u64                        │
│   segment_0_length: u64                                     │
├─────────────────────────────────────────────────────────────┤
│   segment_1_block_index: u64                                │
│   segment_1_uncompressed_offset: u64                        │
│   segment_1_length: u64                                     │
├─────────────────────────────────────────────────────────────┤
│   ...                                                       │
├─────────────────────────────────────────────────────────────┤
│ buffer_2_num_segments: u64                                  │
├─────────────────────────────────────────────────────────────┤
│   segment_0_block_index: u64                                │
│   segment_0_uncompressed_offset: u64                        │
│   segment_0_length: u64                                     │
├─────────────────────────────────────────────────────────────┤
│   ...                                                       │
├─────────────────────────────────────────────────────────────┤
│ buffer_N_num_segments: u64                                  │
├─────────────────────────────────────────────────────────────┤
│   segment_0_block_index: u64                                │
│   segment_0_uncompressed_offset: u64                        │
│   segment_0_length: u64                                     │
├─────────────────────────────────────────────────────────────┤
│   ...                                                       │
└─────────────────────────────────────────────────────────────┘
```

## Memory Layout Details

| **Section** | **Field**              | **Type** | **Size**         | **Description**                       |
|-------------|------------------------|----------|-----------------|---------------------------------------|
| **Header**  | `num_blocks`           | `u64`    | 8 bytes         | Total number of compressed blocks      |
|             | `compressed_sizes[]`   | `u64[]`  | 8 × num_blocks  | Size of each compressed block          |
| **Buffer 1**| `num_segments`         | `u64`    | 8 bytes         | Number of segments for buffer 1        |
|             | `block_index`          | `u64`    | 8 bytes         | Which compressed block contains data   |
|             | `uncompressed_offset`  | `u64`    | 8 bytes         | Offset within uncompressed block       |
|             | `length`               | `u64`    | 8 bytes         | Bytes to read from this block          |
| **Buffer 2**| `num_segments`         | `u64`    | 8 bytes         | Number of segments for buffer 2        |
|             | ...                    | ...      | ...             | Repeat segment pattern                 |
| **Buffer N**| `num_segments`         | `u64`    | 8 bytes         | Number of segments for buffer N        |
|             | ...                    | ...      | ...             | Repeat segment pattern                 |

## Size Calculations

- **Header Size**: `8 + (8 × num_blocks)` bytes  
- **Per Buffer**: `8 + (24 × num_segments)` bytes  
- **Per Segment**: `24` bytes (3 × u64)

## Notes

- All integers are stored in **little-endian** format
- Buffer indices start at **1** (not 0)
- Block indices start at **0**
- Compressed block offsets are calculated by summing previous block sizes
