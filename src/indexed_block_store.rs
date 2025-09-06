use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write, Seek, SeekFrom, Cursor};
use std::thread;
use concurrent_queue::ConcurrentQueue;
use byteorder::{LittleEndian, ReadBytesExt, WriteBytesExt};
use zstd::stream::read::Decoder;

#[derive(Debug)]
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

pub fn pack_buffers(buffers: &[Vec<u8>], block_size: usize) -> (Vec<Vec<u8>>, Vec<Vec<Segment>>) {
    let mut blocks = Vec::new();
    let mut segs = Vec::new();
    let mut cur = Vec::with_capacity(block_size);
    let mut cur_idx = 0usize;

    for buf in buffers {
        let mut rem = buf.as_slice();
        let mut buf_segs = Vec::new();
        while !rem.is_empty() {
            let space = block_size - cur.len();
            let take = rem.len().min(space);
            let off = cur.len();
            cur.extend_from_slice(&rem[..take]);
            buf_segs.push(Segment {
                block_index: cur_idx as u64,
                uncompressed_offset: off as u64,
                length: take as u64,
            });
            rem = &rem[take..];
            if cur.len() == block_size {
                blocks.push(cur);
                cur = Vec::with_capacity(block_size);
                cur_idx += 1;
            }
        }
        segs.push(buf_segs);
    }
    if !cur.is_empty() {
        blocks.push(cur);
    }
    (blocks, segs)
}

pub fn compress_blocks(blocks: Vec<Vec<u8>>) -> io::Result<(Vec<Vec<u8>>, Vec<u64>)> {
    let mut out = Vec::new();
    let mut sizes = Vec::new();
    for b in blocks {
        let comp = zstd::bulk::compress(&b, 0)?;
        sizes.push(comp.len() as u64);
        out.push(comp);
    }
    Ok((out, sizes))
}

pub fn run_pipeline(dat_path: &str, idx_path: &str) -> io::Result<()> {
    let q = ConcurrentQueue::bounded::<Option<Vec<u8>>>(64);
    let prod = q.clone();
    let cons = q.clone();

    // producer
    let producer = thread::spawn(move || {
        for i in 0..200 {
            let buf = vec![i as u8; 2000 + (i % 50)];
            prod.push(Some(buf)).unwrap();
        }
        let _ = prod.push(None);
    });

    // consumer
    let consumer = thread::spawn(move || -> io::Result<()> {
        let mut dat = OpenOptions::new().create(true).write(true).truncate(true).open(dat_path)?;
        let mut idx = OpenOptions::new().create(true).write(true).truncate(true).open(idx_path)?;

        let mut all_sizes = Vec::new();
        let mut all_buffers = Vec::new();

        // placeholder header
        idx.write_u64::<LittleEndian>(0)?;

        while let Ok(opt) = cons.pop() {
            if let Some(buf) = opt {
                all_buffers.push(buf);
                if all_buffers.len() >= 32 {
                    flush_batch(&mut dat, &mut idx, &mut all_sizes, &mut all_buffers)?;
                }
            } else {
                if !all_buffers.is_empty() {
                    flush_batch(&mut dat, &mut idx, &mut all_sizes, &mut all_buffers)?;
                }
                break;
            }
        }

        let end = idx.seek(SeekFrom::Start(0))?;
        idx.write_u64::<LittleEndian>(all_sizes.len() as u64)?;
        for s in &all_sizes {
            idx.write_u64::<LittleEndian>(*s)?;
        }
        idx.seek(SeekFrom::Start(end))?;
        Ok(())
    });

    producer.join().unwrap();
    consumer.join().unwrap()?;
    Ok(())
}

fn flush_batch(
    dat: &mut File,
    idx: &mut File,
    all_sizes: &mut Vec<u64>,
    all_buffers: &mut Vec<Vec<u8>>,
) -> io::Result<()> {
    let (blocks, segs) = pack_buffers(all_buffers, 1 << 20);
    let (comp, sizes) = compress_blocks(blocks)?;
    for b in &comp {
        dat.write_all(b)?;
    }
    all_sizes.extend_from_slice(&sizes);

    for buf in segs {
        idx.write_u64::<LittleEndian>(buf.len() as u64)?;
        for seg in buf {
            idx.write_u64::<LittleEndian>(seg.block_index)?;
            idx.write_u64::<LittleEndian>(seg.uncompressed_offset)?;
            idx.write_u64::<LittleEndian>(seg.length)?;
        }
    }
    all_buffers.clear();
    Ok(())
}

pub fn load_index(path: &str) -> io::Result<Index> {
    let mut f = File::open(path)?;
    let num_blocks = f.read_u64::<LittleEndian>()? as usize;
    let mut sizes = Vec::with_capacity(num_blocks);
    for _ in 0..num_blocks {
        sizes.push(f.read_u64::<LittleEndian>()?);
    }
    let mut buffers = Vec::new();
    while let Ok(num_segments) = f.read_u64::<LittleEndian>() {
        let mut segs = Vec::with_capacity(num_segments as usize);
        for _ in 0..num_segments {
            let b = f.read_u64::<LittleEndian>()?;
            let o = f.read_u64::<LittleEndian>()?;
            let l = f.read_u64::<LittleEndian>()?;
            segs.push(Segment { block_index: b, uncompressed_offset: o, length: l });
        }
        buffers.push(segs);
    }
    Ok(Index { compressed_sizes: sizes, buffers })
}

pub fn recover_buffer_from_files(
    data_path: &str,
    idx: &Index,
    buf_id: usize,
) -> io::Result<Vec<u8>> {
    let mut data_file = File::open(data_path)?;
    let mut offs = Vec::with_capacity(idx.compressed_sizes.len());
    let mut acc = 0u64;
    for s in &idx.compressed_sizes {
        offs.push(acc);
        acc += *s;
    }
    let segs = &idx.buffers[buf_id];
    let mut out = Vec::new();
    for seg in segs {
        let pos = offs[seg.block_index as usize];
        data_file.seek(SeekFrom::Start(pos))?;
        let size = idx.compressed_sizes[seg.block_index as usize];
        let mut comp = vec![0u8; size as usize];
        data_file.read_exact(&mut comp)?;
        let mut dec = Decoder::new(&comp[..])?;
        let mut block = Vec::new();
        dec.read_to_end(&mut block)?;
        out.extend_from_slice(
            &block[seg.uncompressed_offset as usize
                ..(seg.uncompressed_offset + seg.length) as usize],
        );
    }
    Ok(out)
}
