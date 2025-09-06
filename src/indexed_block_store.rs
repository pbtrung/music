use std::thread;
use std::fs::{File, OpenOptions};
use std::io::{self, Cursor, Write, Seek, SeekFrom};
use concurrent_queue::ConcurrentQueue;
use byteorder::{LittleEndian, WriteBytesExt, ReadBytesExt};

const BATCH_SIZE: usize = 5;

pub fn run_pipeline(data_path: &str, index_path: &str) -> io::Result<()> {
    let q = ConcurrentQueue::bounded::<Option<Vec<u8>>>(64);
    let q_prod = q.clone();
    let q_cons = q.clone();

    let data_file = OpenOptions::new().create(true).write(true).truncate(true).open(data_path)?;
    let mut index_file = OpenOptions::new().create(true).write(true).truncate(true).open(index_path)?;

    // reserve header: num_blocks = 0
    index_file.write_u64::<LittleEndian>(0)?; 
    index_file.flush()?;

    let producer = thread::spawn(move || {
        for i in 0..20 {
            let buf = vec![i as u8; 300_000];
            q_prod.push(Some(buf)).unwrap();
        }
        q_prod.push(None).unwrap();
    });

    let consumer = thread::spawn(move || {
        let mut all = Vec::new();
        let mut data_file = data_file;
        let mut index_file = index_file;
        let mut total_blocks = 0u64;
        let mut all_sizes: Vec<u64> = Vec::new();

        loop {
            match q_cons.pop() {
                Ok(Some(buf)) => {
                    all.push(buf);
                    if all.len() >= BATCH_SIZE {
                        let (blocks, segs) = pack_buffers(&all);
                        let (comp, sizes) = compress_blocks(&blocks).unwrap();

                        for c in &comp {
                            data_file.write_all(c).unwrap();
                        }

                        for buf in &segs {
                            index_file.write_u64::<LittleEndian>(buf.len() as u64).unwrap();
                            for seg in buf {
                                index_file.write_u64::<LittleEndian>(seg.block_index + total_blocks).unwrap();
                                index_file.write_u64::<LittleEndian>(seg.uncompressed_offset).unwrap();
                                index_file.write_u64::<LittleEndian>(seg.length).unwrap();
                            }
                        }

                        total_blocks += sizes.len() as u64;
                        all_sizes.extend(sizes);
                        all.clear();
                    }
                }
                Ok(None) => {
                    if !all.is_empty() {
                        let (blocks, segs) = pack_buffers(&all);
                        let (comp, sizes) = compress_blocks(&blocks).unwrap();

                        for c in &comp {
                            data_file.write_all(c).unwrap();
                        }

                        for buf in &segs {
                            index_file.write_u64::<LittleEndian>(buf.len() as u64).unwrap();
                            for seg in buf {
                                index_file.write_u64::<LittleEndian>(seg.block_index + total_blocks).unwrap();
                                index_file.write_u64::<LittleEndian>(seg.uncompressed_offset).unwrap();
                                index_file.write_u64::<LittleEndian>(seg.length).unwrap();
                            }
                        }

                        total_blocks += sizes.len() as u64;
                        all_sizes.extend(sizes);
                        all.clear();
                    }

                    // patch header: write num_blocks + compressed sizes
                    index_file.seek(SeekFrom::Start(0)).unwrap();
                    index_file.write_u64::<LittleEndian>(total_blocks).unwrap();
                    for s in &all_sizes {
                        index_file.write_u64::<LittleEndian>(*s).unwrap();
                    }
                    index_file.flush().unwrap();
                    break;
                }
                Err(_) => continue,
            }
        }
    });

    producer.join().unwrap();
    consumer.join().unwrap();
    Ok(())
}
