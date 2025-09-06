use byteorder::{LittleEndian, ReadBytesExt, WriteBytesExt};
use concurrent_queue::ConcurrentQueue;
use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{self, BufWriter, Read, Seek, SeekFrom, Write};
use std::sync::Arc;
use std::thread;
use zstd::stream::read::Decoder;

#[derive(Debug, Clone)]
pub struct Segment {
    pub block_index: u64,
    pub uncompressed_offset: u64,
    pub length: u64,
}

#[derive(Debug)]
pub struct Index {
    pub compressed_sizes: Vec<u64>,
    pub buffers: Vec<Vec<Segment>>,
}

impl Index {
    pub fn new() -> Self {
        Self {
            compressed_sizes: Vec::new(),
            buffers: Vec::new(),
        }
    }

    // Calculate file offset for a given block index
    pub fn get_block_offset(&self, block_index: usize) -> u64 {
        self.compressed_sizes[..block_index].iter().sum()
    }

    // Get total number of blocks
    pub fn num_blocks(&self) -> usize {
        self.compressed_sizes.len()
    }

    // Get total number of buffers
    pub fn num_buffers(&self) -> usize {
        self.buffers.len()
    }
}

// Configuration for the block store
#[derive(Debug)]
pub struct BlockStoreConfig {
    pub block_size: usize,
    pub batch_size: usize,
    pub compression_level: i32,
    pub queue_size: usize,
}

impl Default for BlockStoreConfig {
    fn default() -> Self {
        Self {
            block_size: 1 << 20, // 1 MiB
            batch_size: 32,
            compression_level: 3, // Better than 0, not too slow
            queue_size: 64,
        }
    }
}

// Pack buffers into blocks, splitting large buffers across blocks if necessary
pub fn pack_buffers(buffers: &[Vec<u8>], block_size: usize) -> (Vec<Vec<u8>>, Vec<Vec<Segment>>) {
    let mut blocks = Vec::new();
    let mut buffer_segments = Vec::new();
    let mut current_block = Vec::with_capacity(block_size);
    let mut current_block_index = 0u64;

    for buffer in buffers {
        let mut remaining = buffer.as_slice();
        let mut segments = Vec::new();

        while !remaining.is_empty() {
            let available_space = block_size - current_block.len();
            let bytes_to_take = remaining.len().min(available_space);
            let offset_in_block = current_block.len();

            // Add data to current block
            current_block.extend_from_slice(&remaining[..bytes_to_take]);

            // Record segment
            segments.push(Segment {
                block_index: current_block_index,
                uncompressed_offset: offset_in_block as u64,
                length: bytes_to_take as u64,
            });

            remaining = &remaining[bytes_to_take..];

            // If block is full, start a new one
            if current_block.len() == block_size {
                blocks.push(current_block);
                current_block = Vec::with_capacity(block_size);
                current_block_index += 1;
            }
        }

        buffer_segments.push(segments);
    }

    // Don't forget the last block if it has data
    if !current_block.is_empty() {
        blocks.push(current_block);
    }

    (blocks, buffer_segments)
}

// Compress blocks using zstd
pub fn compress_blocks(
    blocks: Vec<Vec<u8>>,
    compression_level: i32,
) -> io::Result<(Vec<Vec<u8>>, Vec<u64>)> {
    let mut compressed_blocks = Vec::with_capacity(blocks.len());
    let mut compressed_sizes = Vec::with_capacity(blocks.len());

    for block in blocks {
        let compressed = zstd::bulk::compress(&block, compression_level)?;
        compressed_sizes.push(compressed.len() as u64);
        compressed_blocks.push(compressed);
    }

    Ok((compressed_blocks, compressed_sizes))
}

// Write index file in the documented format
fn write_index_file(
    index_path: &str,
    compressed_sizes: &[u64],
    buffer_segments: &[Vec<Segment>],
) -> io::Result<()> {
    let mut file = BufWriter::new(
        OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(index_path)?,
    );

    // Write header: number of blocks
    file.write_u64::<LittleEndian>(compressed_sizes.len() as u64)?;

    // Write compressed sizes for each block
    for &size in compressed_sizes {
        file.write_u64::<LittleEndian>(size)?;
    }

    // Write buffer segments
    for segments in buffer_segments {
        file.write_u64::<LittleEndian>(segments.len() as u64)?;
        for segment in segments {
            file.write_u64::<LittleEndian>(segment.block_index)?;
            file.write_u64::<LittleEndian>(segment.uncompressed_offset)?;
            file.write_u64::<LittleEndian>(segment.length)?;
        }
    }

    file.flush()?;
    Ok(())
}

// Batch processing for the pipeline
struct BatchProcessor {
    data_file: BufWriter<File>,
    all_compressed_sizes: Vec<u64>,
    all_buffer_segments: Vec<Vec<Segment>>,
    config: BlockStoreConfig,
}

impl BatchProcessor {
    fn new(data_path: &str, config: BlockStoreConfig) -> io::Result<Self> {
        let data_file = BufWriter::new(
            OpenOptions::new()
                .create(true)
                .write(true)
                .truncate(true)
                .open(data_path)?,
        );

        Ok(Self {
            data_file,
            all_compressed_sizes: Vec::new(),
            all_buffer_segments: Vec::new(),
            config,
        })
    }

    fn process_batch(&mut self, buffers: &mut Vec<Vec<u8>>) -> io::Result<()> {
        if buffers.is_empty() {
            return Ok(());
        }

        let (blocks, buffer_segments) = pack_buffers(buffers, self.config.block_size);
        let (compressed_blocks, compressed_sizes) =
            compress_blocks(blocks, self.config.compression_level)?;

        // Write compressed blocks to data file
        for block in &compressed_blocks {
            self.data_file.write_all(block)?;
        }

        // Store metadata for later index writing
        self.all_compressed_sizes
            .extend_from_slice(&compressed_sizes);
        self.all_buffer_segments.extend(buffer_segments);

        buffers.clear();
        Ok(())
    }

    fn finalize(mut self, index_path: &str) -> io::Result<()> {
        self.data_file.flush()?;
        write_index_file(
            index_path,
            &self.all_compressed_sizes,
            &self.all_buffer_segments,
        )?;
        Ok(())
    }
}

// Enhanced pipeline with better error handling and configuration
pub fn run_pipeline_with_config(
    dat_path: &str,
    idx_path: &str,
    config: BlockStoreConfig,
) -> io::Result<()> {
    let queue = Arc::new(ConcurrentQueue::<Option<Vec<u8>>>::bounded(
        config.queue_size,
    ));

    // Convert to owned strings for thread safety
    let dat_path = dat_path.to_string();
    let idx_path = idx_path.to_string();

    let producer_queue = Arc::clone(&queue);
    let consumer_queue = Arc::clone(&queue);
    let batch_size = config.batch_size;

    // Producer thread
    let producer = thread::spawn(move || -> io::Result<()> {
        for i in 0..200 {
            let buffer = vec![i as u8; 2000 + (i % 50)];
            producer_queue
                .push(Some(buffer))
                .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "Queue push failed"))?;
        }
        producer_queue
            .push(None)
            .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "Queue termination failed"))?;
        Ok(())
    });

    // Consumer thread
    let consumer = thread::spawn(move || -> io::Result<()> {
        let mut processor = BatchProcessor::new(&dat_path, config)?;
        let mut buffer_batch = Vec::with_capacity(batch_size);

        loop {
            match consumer_queue.pop() {
                Ok(Some(buffer)) => {
                    buffer_batch.push(buffer);
                    if buffer_batch.len() >= batch_size {
                        processor.process_batch(&mut buffer_batch)?;
                    }
                }
                Ok(None) => {
                    // Process remaining buffers
                    if !buffer_batch.is_empty() {
                        processor.process_batch(&mut buffer_batch)?;
                    }
                    break;
                }
                Err(_) => {
                    return Err(io::Error::new(
                        io::ErrorKind::BrokenPipe,
                        "Queue pop failed",
                    ));
                }
            }
        }

        processor.finalize(&idx_path)?;
        Ok(())
    });

    // Wait for completion
    let producer_result = producer
        .join()
        .map_err(|_| io::Error::new(io::ErrorKind::Other, "Producer thread panicked"))?;
    let consumer_result = consumer
        .join()
        .map_err(|_| io::Error::new(io::ErrorKind::Other, "Consumer thread panicked"))?;

    producer_result?;
    consumer_result?;
    Ok(())
}

// Backward compatibility function
pub fn run_pipeline(dat_path: &str, idx_path: &str) -> io::Result<()> {
    run_pipeline_with_config(dat_path, idx_path, BlockStoreConfig::default())
}

// Load index from file with better error handling
pub fn load_index(path: &str) -> io::Result<Index> {
    let mut file = File::open(path)?;

    // Read header
    let num_blocks = file.read_u64::<LittleEndian>()? as usize;
    let mut compressed_sizes = Vec::with_capacity(num_blocks);

    for _ in 0..num_blocks {
        compressed_sizes.push(file.read_u64::<LittleEndian>()?);
    }

    // Read buffer segments
    let mut buffers = Vec::new();
    while let Ok(num_segments) = file.read_u64::<LittleEndian>() {
        let mut segments = Vec::with_capacity(num_segments as usize);

        for _ in 0..num_segments {
            let block_index = file.read_u64::<LittleEndian>()?;
            let uncompressed_offset = file.read_u64::<LittleEndian>()?;
            let length = file.read_u64::<LittleEndian>()?;

            segments.push(Segment {
                block_index,
                uncompressed_offset,
                length,
            });
        }

        buffers.push(segments);
    }

    Ok(Index {
        compressed_sizes,
        buffers,
    })
}

// Optimized buffer recovery with block caching
pub fn recover_buffer_from_files(
    data_path: &str,
    index: &Index,
    buffer_id: usize,
) -> io::Result<Vec<u8>> {
    if buffer_id >= index.num_buffers() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "Buffer ID {} out of range (max: {})",
                buffer_id,
                index.num_buffers() - 1
            ),
        ));
    }

    let mut data_file = File::open(data_path)?;
    let segments = &index.buffers[buffer_id];
    let mut result = Vec::new();

    // Cache for decompressed blocks to avoid redundant decompression
    let mut block_cache: HashMap<u64, Vec<u8>> = HashMap::new();

    for segment in segments {
        let block_index = segment.block_index as usize;

        if block_index >= index.num_blocks() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Block index {} out of range", block_index),
            ));
        }

        // Get or decompress block
        let decompressed_block = if let Some(cached_block) = block_cache.get(&segment.block_index) {
            cached_block
        } else {
            // Seek to block position
            let block_offset = index.get_block_offset(block_index);
            data_file.seek(SeekFrom::Start(block_offset))?;

            // Read compressed block
            let compressed_size = index.compressed_sizes[block_index] as usize;
            let mut compressed_data = vec![0u8; compressed_size];
            data_file.read_exact(&mut compressed_data)?;

            // Decompress block
            let mut decoder = Decoder::new(&compressed_data[..])?;
            let mut decompressed_data = Vec::new();
            decoder.read_to_end(&mut decompressed_data)?;

            block_cache.insert(segment.block_index, decompressed_data);
            block_cache.get(&segment.block_index).unwrap()
        };

        // Extract segment from decompressed block
        let start = segment.uncompressed_offset as usize;
        let end = start + segment.length as usize;

        if end > decompressed_block.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "Segment extends beyond block boundary: {}..{} > {}",
                    start,
                    end,
                    decompressed_block.len()
                ),
            ));
        }

        result.extend_from_slice(&decompressed_block[start..end]);
    }

    Ok(result)
}

// Utility function to verify index integrity
pub fn verify_index(data_path: &str, index: &Index) -> io::Result<bool> {
    let data_file = File::open(data_path)?;
    let file_size = data_file.metadata()?.len();

    // Check if total compressed size matches file size
    let expected_size: u64 = index.compressed_sizes.iter().sum();
    if expected_size != file_size {
        return Ok(false);
    }

    // Check segment validity
    for (buffer_id, segments) in index.buffers.iter().enumerate() {
        for segment in segments {
            let block_index = segment.block_index as usize;
            if block_index >= index.num_blocks() {
                eprintln!(
                    "Invalid block index in buffer {}: {}",
                    buffer_id, block_index
                );
                return Ok(false);
            }
        }
    }

    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::NamedTempFile;

    #[test]
    fn test_pack_buffers() {
        let buffers = vec![vec![1, 2, 3], vec![4, 5], vec![6, 7, 8, 9]];
        let (blocks, segments) = pack_buffers(&buffers, 5);

        assert_eq!(blocks.len(), 2);
        assert_eq!(blocks[0], vec![1, 2, 3, 4, 5]);
        assert_eq!(blocks[1], vec![6, 7, 8, 9]);
        assert_eq!(segments.len(), 3);
    }

    #[test]
    fn test_index_operations() -> io::Result<()> {
        let temp_data = NamedTempFile::new()?;
        let temp_index = NamedTempFile::new()?;

        run_pipeline(
            temp_data.path().to_str().unwrap(),
            temp_index.path().to_str().unwrap(),
        )?;

        let index = load_index(temp_index.path().to_str().unwrap())?;
        assert!(index.num_blocks() > 0);
        assert!(index.num_buffers() > 0);

        let recovered = recover_buffer_from_files(temp_data.path().to_str().unwrap(), &index, 0)?;
        assert!(!recovered.is_empty());

        Ok(())
    }
}
