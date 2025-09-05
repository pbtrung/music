use anyhow::Result;
use clap::{Parser, Subcommand};
use flexi_logger::{Cleanup, Criterion, DeferredNow, FileSpec, Logger, Naming, Record};
use std::path::PathBuf;

mod config;
use config::Config;

mod audio_decoder;
mod migrate;
mod track;
mod utils;

mod r2duckdb;
use r2duckdb::R2DuckDB;

#[derive(Parser, Debug)]
#[command(name = "music")]
struct Args {
    /// Config file path (JSON format)
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
                "Running migrations using config: {:?}",
                &args.config.display()
            );
            migrate::run_migrate(
                &config.sqlitedb,
                &config.duckdb,
                &config.parquet,
                config.min_value,
            )?;
        }
        Some(Commands::Run) | None => {
            log::info!("Running app with config: {:?}", &args.config.display());
            let r2_db = R2DuckDB::new(config.r2);
            let rand_track_id = utils::generate_unique_ints(1, config.min_value, config.max_value)
                .ok_or(anyhow::anyhow!("Failed to generate random track ID"))?
                .pop()
                .unwrap();
            let track = r2_db.get_track(rand_track_id as u64)?;
            if let Some(track) = track {
                let json = serde_json::to_string_pretty(&track)?;
                println!("{}", json);
            } else {
                println!("No track found with ID: {}", rand_track_id);
            }
        }
    }

    Ok(())
}
