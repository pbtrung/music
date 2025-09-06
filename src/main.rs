use anyhow::Result;
use clap::{Parser, Subcommand};
use flexi_logger::{Cleanup, Criterion, DeferredNow, FileSpec, Logger, Naming, Record};
use std::path::PathBuf;

mod config;
use config::Config;

mod audio_decoder;
mod indexed_block_store;
mod track;
mod utils;

#[derive(Parser, Debug)]
#[command(name = "music")]
struct Args {
    #[arg(short, long, default_value = "config.json")]
    config: PathBuf,

    #[command(subcommand)]
    command: Option<Commands>,
}

#[derive(Subcommand, Debug)]
enum Commands {
    Migrate,
    Run,
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
                .directory(&config.log_dir)
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

    match args.command {
        Some(Commands::Migrate) => {
            log::info!(
                "Running migration using config: {:?}",
                &args.config.display()
            );
        }
        Some(Commands::Run) | None => {
            log::info!("Running app with config: {:?}", &args.config.display());
        }
    }

    Ok(())
}
