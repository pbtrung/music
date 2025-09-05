use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct R2Config {
    pub access_key: String,
    pub secret_key: String,
    pub account_id: String,
    pub db_file: String,
    pub bucket: String,
}

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct CosmosDbConfig {
    pub cosmos_uri: String,
    pub cosmos_key: String,
    pub cosmos_db_name: String,
    pub cosmos_container: String,
    pub partition_key_path: String,
    pub restart_track_id: i32,
}

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct GdrAccount {
    pub email: String,
    pub client_id: String,
    pub client_secret: String,
    pub refresh_token: String,
}

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct Config {
    pub sqlitedb: String,
    pub duckdb: String,
    pub parquet: String,
    pub download_dir: String,
    pub max_retries: i32,
    pub timeout: i32,
    pub num_files: i32,
    pub pipe: String,
    pub min_value: i32,
    pub max_value: i32,
    pub log_dir: String,
    pub n_gateway: String,
    pub i_gateway: String,
    pub ncores: i32,
    pub mul_factor: i32,
    pub cosmosdb: CosmosDbConfig,
    pub r2: R2Config,
    pub gdr_accounts: Vec<GdrAccount>,
    pub gateways: Vec<String>,
}

impl Config {
    pub fn from_file<P: AsRef<Path>>(path: P) -> Result<Self> {
        let content = fs::read_to_string(&path)
            .with_context(|| format!("Failed to read config file: {}", path.as_ref().display()))?;

        let config: Config =
            serde_json::from_str(&content).with_context(|| "Failed to parse JSON config")?;

        Ok(config)
    }
}
