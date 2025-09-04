use anyhow::Result;
use clap::Parser;
use flexi_logger::{Cleanup, Criterion, DeferredNow, FileSpec, Logger, Naming, Record};
use std::path::PathBuf;

mod config;
use config::Config;

mod audio_decoder;
use audio_decoder::AudioDecoder;

mod utils;

#[derive(Parser, Debug)]
#[command(name = "music")]
struct Args {
    /// Config file path (JSON format)
    #[arg(short, long, default_value = "config.json")]
    config: PathBuf,
}

fn log_format(
    w: &mut dyn std::io::Write,
    now: &mut DeferredNow,
    record: &Record,
) -> std::io::Result<()> {
    write!(
        w,
        "[{}] {:<5}[{}:{}] {}",
        now.now().format("%Y-%m-%d %H:%M:%S%.3f"),
        record.level(),
        record.target(),
        record.line().unwrap_or(0),
        record.args()
    )
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();
    let config = Config::from_file(&args.config)?;

    Logger::try_with_str("info")?
        .log_to_file(
            FileSpec::default()
                .directory(config.log_dir)
                .basename("music")
                .suffix("log"),
        )
        .rotate(
            Criterion::Size(15_000_000),
            Naming::Numbers,
            Cleanup::KeepLogFiles(4),
        )
        .format_for_files(log_format)
        .start()?;

    let input = "test.opus";
    let mut decoder = AudioDecoder::new(config.pipe, input.to_string())?;

    log::info!("Starting audio decoding for: {}", input);
    decoder.decode().await?;
    log::info!("Audio decoding completed successfully");

    Ok(())
}
