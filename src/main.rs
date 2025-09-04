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
        "[{}] {:<5}[{}] {}",
        now.now().format("%Y-%m-%d %H:%M:%S"),
        record.level(),
        record.target(),
        record.args()
    )
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();

    Logger::try_with_str("info")?
        .log_to_file(
            FileSpec::default()
                .directory("logs")
                .basename("music")
                .suffix("log"),
        )
        .rotate(
            Criterion::Size(10_000_000),
            Naming::Numbers,
            Cleanup::KeepLogFiles(4),
        )
        .format_for_files(log_format)
        .start()?;

    let config = Config::from_file(&args.config)?;
    log::info!("Loaded config from: {}", args.config.display());

    let input = "test.opus";
    let mut decoder = AudioDecoder::new(config.pipe_name, input.to_string())?;

    log::info!("Starting audio decoding for: {}", input);
    decoder.decode().await?;
    log::info!("Audio decoding completed successfully");

    Ok(())
}
