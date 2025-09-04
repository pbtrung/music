use anyhow::{Context, Result, anyhow};
use rusty_ffmpeg::ffi;
use std::ffi::{CStr, CString};
use std::fs::File;
use std::io::{BufWriter, Write};
use std::ptr;
use std::time::Instant;

use crate::utils;

struct PacketGuard(*mut ffi::AVPacket);
impl PacketGuard {
    fn new() -> Result<Self> {
        let packet = unsafe { ffi::av_packet_alloc() };
        if packet.is_null() {
            return Err(anyhow!("Failed to allocate packet"));
        }
        Ok(PacketGuard(packet))
    }

    fn as_ptr(&self) -> *mut ffi::AVPacket {
        self.0
    }
}

impl Drop for PacketGuard {
    fn drop(&mut self) {
        unsafe {
            if !self.0.is_null() {
                ffi::av_packet_free(&mut self.0 as *mut _);
            }
        }
    }
}

struct FrameGuard(*mut ffi::AVFrame);
impl FrameGuard {
    fn new() -> Result<Self> {
        let frame = unsafe { ffi::av_frame_alloc() };
        if frame.is_null() {
            return Err(anyhow!("Failed to allocate frame"));
        }
        Ok(FrameGuard(frame))
    }

    fn as_ptr(&self) -> *mut ffi::AVFrame {
        self.0
    }
}

impl Drop for FrameGuard {
    fn drop(&mut self) {
        unsafe {
            if !self.0.is_null() {
                ffi::av_frame_free(&mut self.0 as *mut _);
            }
        }
    }
}

struct ChannelLayoutGuard(ffi::AVChannelLayout);
impl ChannelLayoutGuard {
    fn new(channels: i32) -> Self {
        unsafe {
            let mut ch_layout: ffi::AVChannelLayout = std::mem::zeroed();
            ffi::av_channel_layout_default(&mut ch_layout, channels);
            ChannelLayoutGuard(ch_layout)
        }
    }

    fn as_ptr(&self) -> *const ffi::AVChannelLayout {
        &self.0 as *const _
    }
}

impl Drop for ChannelLayoutGuard {
    fn drop(&mut self) {
        unsafe {
            ffi::av_channel_layout_uninit(&mut self.0);
        }
    }
}

pub struct AudioDecoder {
    pipe: String,
    path: String,
    rate: i32,
    channels: i32,
    format: ffi::AVSampleFormat,
    fmt_ctx: *mut ffi::AVFormatContext,
    codec_ctx: *mut ffi::AVCodecContext,
    swr_ctx: *mut ffi::SwrContext,
    stream_idx: i32,
    output: Option<BufWriter<File>>,
}

impl AudioDecoder {
    pub fn new(pipe: String, path: String) -> Result<Self> {
        unsafe {
            ffi::av_log_set_level(ffi::AV_LOG_ERROR as i32);
        }
        log::info!(
            "AudioDecoder created with pipe {} and path {}",
            &pipe,
            &path,
        );

        Ok(Self {
            pipe,
            path,
            rate: 48000,
            channels: 2,
            format: ffi::AV_SAMPLE_FMT_S16,
            fmt_ctx: ptr::null_mut(),
            codec_ctx: ptr::null_mut(),
            swr_ctx: ptr::null_mut(),
            stream_idx: -1,
            output: None,
        })
    }

    pub async fn decode(&mut self) -> Result<()> {
        log::info!("Starting decode operation for {}", self.path);
        let start = Instant::now();

        self.setup().await?;
        log::info!(
            "Setup completed in {:.1}ms",
            start.elapsed().as_secs_f64() * 1000.0,
        );

        self.run_decode().await?;
        log::info!("Decode operation completed for {}", self.path);
        Ok(())
    }

    async fn setup(&mut self) -> Result<()> {
        self.open_file()?;
        self.get_stream_info()?;
        self.show_metadata();
        self.find_audio()?;
        self.setup_codec()?;
        self.setup_resampler()?;
        self.create_output()?;
        self.show_audio_info();
        Ok(())
    }

    fn open_file(&mut self) -> Result<()> {
        log::info!("Opening input file {}", self.path);
        let c_path = CString::new(self.path.clone())
            .map_err(|_| anyhow!("Invalid path contains null bytes"))?;

        unsafe {
            let ret = ffi::avformat_open_input(
                &mut self.fmt_ctx,
                c_path.as_ptr(),
                ptr::null_mut(),
                ptr::null_mut(),
            );
            if ret < 0 {
                return Err(anyhow!("Failed to open {}: error code {}", self.path, ret));
            }
        }
        log::info!("Input file opened successfully");
        Ok(())
    }

    fn get_stream_info(&mut self) -> Result<()> {
        log::info!("Analyzing stream information");
        unsafe {
            let ret = ffi::avformat_find_stream_info(self.fmt_ctx, ptr::null_mut());
            if ret < 0 {
                return Err(anyhow!("Failed to analyze streams: error code {}", ret));
            }
        }
        log::info!("Stream information analyzed");
        Ok(())
    }

    fn show_metadata(&self) {
        log::info!("Displaying file metadata");
        unsafe {
            if self.fmt_ctx.is_null() {
                return;
            }
            let metadata = (*self.fmt_ctx).metadata;
            let mut entry = ptr::null_mut();

            println!("File metadata:");
            loop {
                let empty_str = match CString::new("") {
                    Ok(s) => s,
                    Err(_) => return,
                };

                entry = ffi::av_dict_get(
                    metadata,
                    empty_str.as_ptr(),
                    entry,
                    ffi::AV_DICT_IGNORE_SUFFIX as i32,
                );
                if entry.is_null() {
                    break;
                }

                let key = CStr::from_ptr((*entry).key).to_string_lossy();
                let value = CStr::from_ptr((*entry).value).to_string_lossy();
                println!("  {}: {}", key, value);
            }
        }
    }

    fn find_audio(&mut self) -> Result<()> {
        log::info!("Searching for audio stream");
        unsafe {
            let count = (*self.fmt_ctx).nb_streams;
            let streams = (*self.fmt_ctx).streams;

            for i in 0..count {
                let stream = *streams.add(i as usize);
                let params = (*stream).codecpar;

                if (*params).codec_type == ffi::AVMEDIA_TYPE_AUDIO {
                    self.stream_idx = i as i32;
                    break;
                }
            }

            if self.stream_idx == -1 {
                return Err(anyhow!("No audio stream found"));
            }
        }
        log::info!("Found audio stream at index {}", self.stream_idx);
        Ok(())
    }

    fn setup_codec(&mut self) -> Result<()> {
        log::info!("Setting up audio codec");
        unsafe {
            let streams = (*self.fmt_ctx).streams;
            let stream = *streams.add(self.stream_idx as usize);
            let params = (*stream).codecpar;

            let codec = ffi::avcodec_find_decoder((*params).codec_id);
            if codec.is_null() {
                return Err(anyhow!(
                    "Decoder not found for codec ID {}",
                    (*params).codec_id
                ));
            }

            self.codec_ctx = ffi::avcodec_alloc_context3(codec);
            if self.codec_ctx.is_null() {
                return Err(anyhow!("Failed to allocate codec context"));
            }

            let ret = ffi::avcodec_parameters_to_context(self.codec_ctx, params);
            if ret < 0 {
                // Cleanup on error
                ffi::avcodec_free_context(&mut self.codec_ctx);
                return Err(anyhow!(
                    "Failed to copy codec parameters: error code {}",
                    ret
                ));
            }

            (*self.codec_ctx).pkt_timebase = (*stream).time_base;

            let ret = ffi::avcodec_open2(self.codec_ctx, codec, ptr::null_mut());
            if ret < 0 {
                // Cleanup on error
                ffi::avcodec_free_context(&mut self.codec_ctx);
                return Err(anyhow!("Failed to open codec: error code {}", ret));
            }
        }
        log::info!("Audio codec setup completed");
        Ok(())
    }

    fn setup_resampler(&mut self) -> Result<()> {
        log::info!("Configuring audio resampler");
        unsafe {
            self.swr_ctx = ffi::swr_alloc();
            if self.swr_ctx.is_null() {
                return Err(anyhow!("Failed to allocate resampler"));
            }

            // If any step fails, we need to cleanup
            if let Err(e) = self.set_input_params() {
                ffi::swr_free(&mut self.swr_ctx);
                return Err(e);
            }

            if let Err(e) = self.set_output_params() {
                ffi::swr_free(&mut self.swr_ctx);
                return Err(e);
            }

            let ret = ffi::swr_init(self.swr_ctx);
            if ret < 0 {
                ffi::swr_free(&mut self.swr_ctx);
                return Err(anyhow!(
                    "Failed to initialize resampler: error code {}",
                    ret
                ));
            }
        }
        log::info!("Audio resampler configured");
        Ok(())
    }

    fn set_input_params(&mut self) -> Result<()> {
        unsafe {
            let ctx = self.swr_ctx as *mut std::ffi::c_void;

            let in_chlayout_str =
                CString::new("in_chlayout").map_err(|_| anyhow!("Failed to create C string"))?;
            let in_sample_rate_str =
                CString::new("in_sample_rate").map_err(|_| anyhow!("Failed to create C string"))?;
            let in_sample_fmt_str =
                CString::new("in_sample_fmt").map_err(|_| anyhow!("Failed to create C string"))?;

            let ret = ffi::av_opt_set_chlayout(
                ctx,
                in_chlayout_str.as_ptr(),
                &(*self.codec_ctx).ch_layout,
                0,
            );
            if ret < 0 {
                return Err(anyhow!(
                    "Failed to set input channel layout: error code {}",
                    ret
                ));
            }

            let ret = ffi::av_opt_set_int(
                ctx,
                in_sample_rate_str.as_ptr(),
                (*self.codec_ctx).sample_rate as i64,
                0,
            );
            if ret < 0 {
                return Err(anyhow!(
                    "Failed to set input sample rate: error code {}",
                    ret
                ));
            }

            let ret = ffi::av_opt_set_sample_fmt(
                ctx,
                in_sample_fmt_str.as_ptr(),
                (*self.codec_ctx).sample_fmt,
                0,
            );
            if ret < 0 {
                return Err(anyhow!(
                    "Failed to set input sample format: error code {}",
                    ret
                ));
            }
        }
        Ok(())
    }

    fn set_output_params(&mut self) -> Result<()> {
        unsafe {
            let ctx = self.swr_ctx as *mut std::ffi::c_void;
            let ch_layout = ChannelLayoutGuard::new(self.channels);

            let out_chlayout_str =
                CString::new("out_chlayout").map_err(|_| anyhow!("Failed to create C string"))?;
            let out_sample_rate_str = CString::new("out_sample_rate")
                .map_err(|_| anyhow!("Failed to create C string"))?;
            let out_sample_fmt_str =
                CString::new("out_sample_fmt").map_err(|_| anyhow!("Failed to create C string"))?;

            let ret =
                ffi::av_opt_set_chlayout(ctx, out_chlayout_str.as_ptr(), ch_layout.as_ptr(), 0);
            if ret < 0 {
                return Err(anyhow!(
                    "Failed to set output channel layout: error code {}",
                    ret
                ));
            }

            let ret = ffi::av_opt_set_int(ctx, out_sample_rate_str.as_ptr(), self.rate as i64, 0);
            if ret < 0 {
                return Err(anyhow!(
                    "Failed to set output sample rate: error code {}",
                    ret
                ));
            }

            let ret = ffi::av_opt_set_sample_fmt(ctx, out_sample_fmt_str.as_ptr(), self.format, 0);
            if ret < 0 {
                return Err(anyhow!(
                    "Failed to set output sample format: error code {}",
                    ret
                ));
            }
        }
        Ok(())
    }

    fn create_output(&mut self) -> Result<()> {
        log::info!("Creating output file {}", self.pipe);
        let file =
            File::create(&self.pipe).with_context(|| format!("Failed to create {}", self.pipe))?;
        self.output = Some(BufWriter::new(file));
        log::info!("Output file created");
        Ok(())
    }

    fn show_audio_info(&self) {
        log::info!("Displaying audio stream information");
        unsafe {
            if self.codec_ctx.is_null() {
                return;
            }

            let codec = (*self.codec_ctx).codec;
            if !codec.is_null() {
                let name = CStr::from_ptr((*codec).long_name).to_string_lossy();
                println!("  codec: {}", name);
            }

            if (*self.codec_ctx).bit_rate != 0 {
                println!("  bitrate: {} kbps", (*self.codec_ctx).bit_rate / 1000);
            }

            let in_rate = (*self.codec_ctx).sample_rate;
            let in_channels = (*self.codec_ctx).ch_layout.nb_channels;

            println!("  sample rate: {}", in_rate);
            println!("  channels: {}", in_channels);

            if in_channels != self.channels {
                println!("  channel conversion: {} -> {}", in_channels, self.channels);
            }
            if in_rate != self.rate {
                println!("  sample rate conversion: {} -> {}", in_rate, self.rate);
            }
        }
    }

    async fn run_decode(&mut self) -> Result<()> {
        log::info!("Starting decode loop");

        // Use RAII guards for automatic cleanup
        let packet_guard = PacketGuard::new()?;
        let frame_guard = FrameGuard::new()?;

        let packet = packet_guard.as_ptr();
        let frame = frame_guard.as_ptr();

        let duration = self.get_duration();
        let duration_str = utils::format_time(duration.unwrap_or(-1.0));

        unsafe {
            while ffi::av_read_frame(self.fmt_ctx, packet) >= 0 {
                if (*packet).stream_index == self.stream_idx {
                    self.handle_packet(packet, frame, &duration_str).await?;
                }
                ffi::av_packet_unref(packet);
            }

            self.flush_decoder(frame, &duration_str).await?;
        }

        println!("\nDecoding completed");
        log::info!("Decode loop finished");
        Ok(())
    }

    fn get_duration(&self) -> Option<f64> {
        unsafe {
            let streams = (*self.fmt_ctx).streams;
            let stream = *streams.add(self.stream_idx as usize);

            if (*stream).duration != ffi::AV_NOPTS_VALUE {
                let tb = (*stream).time_base;
                Some((*stream).duration as f64 * ffi::av_q2d(tb))
            } else if (*self.fmt_ctx).duration != ffi::AV_NOPTS_VALUE {
                Some((*self.fmt_ctx).duration as f64 / ffi::AV_TIME_BASE as f64)
            } else {
                None
            }
        }
    }

    async fn handle_packet(
        &mut self,
        packet: *mut ffi::AVPacket,
        frame: *mut ffi::AVFrame,
        duration: &str,
    ) -> Result<()> {
        unsafe {
            let ret = ffi::avcodec_send_packet(self.codec_ctx, packet);
            if ret < 0 {
                return Err(anyhow!("Failed to send packet: error code {}", ret));
            }
            self.receive_frames(frame, duration).await
        }
    }

    async fn flush_decoder(&mut self, frame: *mut ffi::AVFrame, duration: &str) -> Result<()> {
        log::info!("Flushing decoder");
        unsafe {
            let ret = ffi::avcodec_send_packet(self.codec_ctx, ptr::null_mut());
            if ret < 0 {
                return Err(anyhow!("Failed to flush decoder: error code {}", ret));
            }
            self.receive_frames(frame, duration).await
        }
    }

    async fn receive_frames(&mut self, frame: *mut ffi::AVFrame, duration: &str) -> Result<()> {
        unsafe {
            loop {
                let ret = ffi::avcodec_receive_frame(self.codec_ctx, frame);
                match ret {
                    r if r == ffi::AVERROR(ffi::EAGAIN) || r == ffi::AVERROR_EOF => break,
                    r if r < 0 => return Err(anyhow!("Decode error: error code {}", r)),
                    _ => self.process_frame(frame, duration).await?,
                }
            }
        }
        Ok(())
    }

    async fn process_frame(&mut self, frame: *mut ffi::AVFrame, duration: &str) -> Result<()> {
        unsafe {
            let samples = (*frame).nb_samples;
            let max_out = ffi::av_rescale_rnd(
                samples as i64,
                self.rate as i64,
                (*self.codec_ctx).sample_rate as i64,
                ffi::AV_ROUND_UP,
            ) as i32;

            let bytes_per_sample = ffi::av_get_bytes_per_sample(self.format);
            let buffer_size = (max_out * self.channels * bytes_per_sample) as usize;
            let mut buffer = vec![0u8; buffer_size];
            let mut output_data = [buffer.as_mut_ptr(); 8];

            let converted = ffi::swr_convert(
                self.swr_ctx,
                output_data.as_mut_ptr(),
                max_out,
                (*frame).data.as_ptr() as *const *const u8,
                samples,
            );

            if converted < 0 {
                return Err(anyhow!("Conversion failed: error code {}", converted));
            }

            let size = (converted * self.channels * bytes_per_sample) as usize;
            if size > 0 && size <= buffer_size {
                if let Some(ref mut out) = self.output {
                    out.write_all(&buffer[..size])
                        .with_context(|| "Failed to write output data")?;
                }
            }

            self.show_progress(frame, duration)?;
        }
        Ok(())
    }

    fn show_progress(&self, frame: *mut ffi::AVFrame, duration: &str) -> Result<()> {
        unsafe {
            if (*frame).pts != ffi::AV_NOPTS_VALUE {
                let streams = (*self.fmt_ctx).streams;
                let stream = *streams.add(self.stream_idx as usize);
                let tb = (*stream).time_base;
                let current = (*frame).pts as f64 * ffi::av_q2d(tb);
                let time_str = utils::format_time(current);

                print!("  progress: {} / {}\r", time_str, duration);
                std::io::stdout().flush()?;
            }
        }
        Ok(())
    }
}

impl Drop for AudioDecoder {
    fn drop(&mut self) {
        log::info!("Cleaning up AudioDecoder resources");
        unsafe {
            if !self.swr_ctx.is_null() {
                ffi::swr_free(&mut self.swr_ctx);
            }
            if !self.codec_ctx.is_null() {
                ffi::avcodec_free_context(&mut self.codec_ctx);
            }
            if !self.fmt_ctx.is_null() {
                ffi::avformat_close_input(&mut self.fmt_ctx);
            }
        }
        log::info!("AudioDecoder cleanup completed");
    }
}
