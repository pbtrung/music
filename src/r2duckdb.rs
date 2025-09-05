use anyhow::{Context, Result, bail};
use duckdb::{Connection, Result as DuckResult, Row};

use crate::config::R2Config;
use crate::track::Track;

pub struct R2DuckDB {
    pub access_key: String,
    pub secret_key: String,
    pub account_id: String,
    pub bucket: String,
    pub db_file: String,
}

impl R2DuckDB {
    pub fn new(config: R2Config) -> Self {
        Self {
            access_key: config.access_key,
            secret_key: config.secret_key,
            account_id: config.account_id,
            bucket: config.bucket,
            db_file: config.db_file,
        }
    }

    pub fn query<T, F>(&self, sql: &str, map_row: F) -> Result<Vec<T>>
    where
        F: Fn(&Row) -> DuckResult<T>,
    {
        self.validate_inputs(sql)?;
        let conn = self.connect()?;
        let results = self.execute_query(&conn, &sql, map_row)?;
        Ok(results)
    }

    fn validate_inputs(&self, sql: &str) -> Result<()> {
        if sql.trim().is_empty() {
            bail!("SQL query cannot be empty");
        }
        let sql_upper = sql.to_uppercase();
        if !sql_upper.trim_start().starts_with("SELECT")
            && !sql_upper.trim_start().starts_with("WITH")
            && !sql_upper.trim_start().starts_with("SHOW")
            && !sql_upper.trim_start().starts_with("DESCRIBE")
        {
            bail!("Only SELECT, WITH, SHOW, and DESCRIBE statements are allowed");
        }
        for keyword in [
            "DROP", "DELETE", "INSERT", "UPDATE", "CREATE", "ALTER", "TRUNCATE",
        ] {
            if sql_upper.contains(keyword) {
                bail!("Dangerous SQL keyword detected: {}", keyword);
            }
        }
        if [
            self.access_key.as_str(),
            self.secret_key.as_str(),
            self.account_id.as_str(),
            self.bucket.as_str(),
            self.db_file.as_str(),
        ]
        .iter()
        .any(|s| s.trim().is_empty())
        {
            bail!("All connection parameters must be non-empty");
        }
        Ok(())
    }

    fn connect(&self) -> Result<Connection> {
        let conn =
            Connection::open_in_memory().context("Failed to create in-memory DuckDB connection")?;

        conn.execute(
            &format!(
                "CREATE SECRET r2 (TYPE r2, KEY_ID '{}', SECRET '{}', ACCOUNT_ID '{}');",
                self.access_key, self.secret_key, self.account_id
            ),
            [],
        )
        .context("Failed to create secret")?;

        Ok(conn)
    }

    fn execute_query<T, F>(&self, conn: &Connection, query: &str, map_row: F) -> Result<Vec<T>>
    where
        F: Fn(&Row) -> DuckResult<T>,
    {
        let mut stmt = conn
            .prepare(query)
            .context("Failed to prepare SQL statement")?;
        let mapped_rows = stmt
            .query_map([], map_row)
            .context("Failed to execute query")?;
        let mut results = Vec::new();
        for (i, row) in mapped_rows.enumerate() {
            results.push(row.with_context(|| format!("Failed to process row {}", i))?);
        }
        Ok(results)
    }

    pub fn get_track(&self, track_id: u64) -> Result<Option<Track>> {
        let sql = format!(
            "SELECT track FROM 'r2://{}/{}' WHERE track_id = {} LIMIT 1;",
            self.bucket, self.db_file, track_id
        );

        log::info!("sql: \"{}\"", sql);
        let results = self.query(&sql, |row| {
            let track_json: String = row.get(0)?;
            let track: Track = serde_json::from_str(&track_json).map_err(|e| {
                duckdb::Error::FromSqlConversionFailure(0, duckdb::types::Type::Text, Box::new(e))
            })?;

            Ok(track)
        })?;

        Ok(results.into_iter().next())
    }
}
