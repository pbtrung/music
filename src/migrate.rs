use anyhow::{Context, Result};
use duckdb::Connection as DuckConn;
use rusqlite::{Connection as SqliteConn, Row, params};
use serde_json::{Value, json};
use std::collections::HashMap;
use std::time::Instant;

#[derive(Debug)]
struct TrackRow {
    track_id: i64,
    track_name: String,
    album_id: i64,
    path: String,
    content_cid: Option<String>,
    gdr_cid: Option<String>,
    start_byte: Option<i64>,
    end_byte: Option<i64>,
    gdr_account_id: Option<i64>,
    email: Option<String>,
}

impl TrackRow {
    fn from_row(row: &Row) -> Result<Self> {
        Ok(TrackRow {
            track_id: row.get(0).context("Failed to get track_id")?,
            track_name: row.get(1).context("Failed to get track_name")?,
            album_id: row.get(2).context("Failed to get album_id")?,
            path: row.get(3).context("Failed to get path")?,
            content_cid: row.get(4).ok(),
            gdr_cid: row.get(5).ok(),
            start_byte: row.get(6).ok(),
            end_byte: row.get(7).ok(),
            gdr_account_id: row.get(8).ok(),
            email: row.get(9).ok(),
        })
    }

    fn validate(&self) -> bool {
        if self.track_id < 0 {
            log::warn!("Skipping track with negative ID: {}", self.track_id);
            return false;
        }

        if let (Some(start), Some(end)) = (self.start_byte, self.end_byte) {
            if start < 0 || end < 0 || start > end {
                log::warn!(
                    "Invalid byte range for track {}: start={}, end={}",
                    self.track_id,
                    start,
                    end
                );
            }
        }

        true
    }
}

fn setup_sqlite_connection(sqlite_path: &str) -> Result<SqliteConn> {
    let sqlite = SqliteConn::open(sqlite_path)
        .with_context(|| format!("Failed to open SQLite database at: {}", sqlite_path))?;

    sqlite
        .execute_batch(
            "
        PRAGMA synchronous = OFF;
        PRAGMA journal_mode = MEMORY;
        PRAGMA temp_store = MEMORY;
        PRAGMA cache_size = 10000;
        PRAGMA mmap_size = 268435456;
        ",
        )
        .context("Failed to set SQLite optimization pragmas")?;

    Ok(sqlite)
}

fn setup_duckdb_connection(duckdb_path: &str) -> Result<DuckConn> {
    let mut duck = DuckConn::open(duckdb_path)
        .with_context(|| format!("Failed to open DuckDB database at: {}", duckdb_path))?;

    duck.execute("SET default_block_size=131072", [])
        .context("Failed to set default block size")?;
    duck.execute("SET memory_limit='8GB'", [])
        .context("Failed to set memory limit")?;
    duck.execute(
        "CREATE TABLE IF NOT EXISTS tracks (
            track_id INTEGER PRIMARY KEY,
            track JSON
        )",
        [],
    )
    .context("Failed to create tracks table in DuckDB")?;

    Ok(duck)
}

fn process_tracks_batch(
    sqlite: &SqliteConn,
    duck: &mut DuckConn,
    batch_start: i64,
    batch_size: i64,
) -> Result<usize> {
    // Read batch from SQLite
    let mut stmt = sqlite
        .prepare(
            r#"
        SELECT 
            t.track_id, 
            t.track_name, 
            a.album_id, 
            a.path,
            cc.cid as content_cid,
            cg.cid as gdr_cid,
            cg.start_byte,
            cg.end_byte,
            cg.gdr_account_id,
            ga.email
        FROM tracks t
        JOIN albums a ON t.album_id = a.album_id
        LEFT JOIN content_cid cc ON t.track_id = cc.track_id
        LEFT JOIN content_gdr cg ON t.track_id = cg.track_id
        LEFT JOIN gdr_accounts ga ON cg.gdr_account_id = ga.gdr_account_id
        WHERE t.track_id >= ? AND t.track_id < ?
        ORDER BY t.track_id, cc.content_id, cg.content_id
        "#,
        )
        .context("Failed to prepare SQLite query")?;

    let mut rows = stmt
        .query(params![batch_start, batch_start + batch_size])
        .context("Failed to execute SQLite query")?;

    // Process batch in memory
    let mut tracks: HashMap<i64, Value> = HashMap::new();
    let mut row_count = 0;

    while let Some(row) = rows.next()? {
        let track_row = TrackRow::from_row(row)?;

        if !track_row.validate() {
            continue;
        }

        process_track_row(&mut tracks, track_row)?;
        row_count += 1;
    }

    // Insert batch into DuckDB
    if !tracks.is_empty() {
        let inserted = insert_tracks_batch_to_duckdb(duck, tracks)?;
        log::info!(
            "Processed batch {}-{}: {} rows read, {} unique tracks inserted",
            batch_start,
            batch_start + batch_size - 1,
            row_count,
            inserted
        );
        Ok(inserted)
    } else {
        Ok(0)
    }
}

fn process_track_row(tracks: &mut HashMap<i64, Value>, row: TrackRow) -> Result<()> {
    let track_entry = tracks.entry(row.track_id).or_insert_with(|| {
        json!({
            "track_id": row.track_id,
            "track_name": row.track_name,
            "album": {
                "album_id": row.album_id,
                "path": row.path,
            },
            "cids": []
        })
    });

    // Handle both content_cid and gdr_cid
    add_cid_to_track(track_entry, row.content_cid)?;
    add_cid_to_track(track_entry, row.gdr_cid)?;

    // Handle GDR fields (1-to-1 relationship)
    set_gdr_fields(
        track_entry,
        row.track_id,
        row.start_byte,
        row.end_byte,
        row.gdr_account_id,
        row.email,
    )?;

    Ok(())
}

fn add_cid_to_track(track_entry: &mut Value, cid: Option<String>) -> Result<()> {
    if let Some(cid_value) = cid {
        if !cid_value.trim().is_empty() {
            let cids = track_entry["cids"]
                .as_array_mut()
                .context("cids field is not an array")?;

            let cid_json = json!(cid_value.trim());

            // Check if this exact CID already exists (avoid duplicates)
            if !cids.contains(&cid_json) {
                cids.push(cid_json);
            }
        }
    }
    Ok(())
}

fn set_gdr_fields(
    track_entry: &mut Value,
    track_id: i64,
    start_byte: Option<i64>,
    end_byte: Option<i64>,
    gdr_account_id: Option<i64>,
    email: Option<String>,
) -> Result<()> {
    // Handle byte range
    if let (Some(start), Some(end)) = (start_byte, end_byte) {
        if start >= 0 && end >= 0 && start <= end {
            if let Some(existing_range) = track_entry.get("byte_range") {
                let existing = existing_range
                    .as_array()
                    .context("existing byte_range is not an array")?;
                let existing_start = existing[0]
                    .as_u64()
                    .context("existing start byte is not a number")?
                    as i64;
                let existing_end = existing[1]
                    .as_u64()
                    .context("existing end byte is not a number")?
                    as i64;
                if existing_start != start || existing_end != end {
                    log::warn!(
                        "Conflicting byte_range for track {}: existing=[{}, {}], new=[{}, {}]",
                        track_id,
                        existing_start,
                        existing_end,
                        start,
                        end
                    );
                }
            } else {
                track_entry["byte_range"] = json!([start as u64, end as u64]);
            }
        }
    }

    // Handle GDR account ID
    if let Some(acc) = gdr_account_id {
        if acc >= 0 {
            if let Some(existing_acc) = track_entry.get("gdr_account_id") {
                let existing_id = existing_acc
                    .as_i64()
                    .context("existing gdr_account_id is not a number")?;
                if existing_id != acc {
                    log::warn!(
                        "Conflicting gdr_account_id for track {}: existing={}, new={}",
                        track_id,
                        existing_id,
                        acc
                    );
                }
            } else {
                track_entry["gdr_account_id"] = json!(acc);
            }
        }
    }

    // Handle email
    if let Some(mail) = email {
        if !mail.trim().is_empty() {
            if let Some(existing_email) = track_entry.get("email") {
                let existing_str = existing_email
                    .as_str()
                    .context("existing email is not a string")?;
                if existing_str != mail.trim() {
                    log::warn!(
                        "Conflicting email for track {}: existing='{}', new='{}'",
                        track_id,
                        existing_str,
                        mail.trim()
                    );
                }
            } else {
                track_entry["email"] = json!(mail.trim());
            }
        }
    }

    Ok(())
}

fn insert_tracks_batch_to_duckdb(
    duck: &mut DuckConn,
    tracks: HashMap<i64, Value>,
) -> Result<usize> {
    let mut tx = duck
        .transaction()
        .context("Failed to start DuckDB transaction")?;

    let mut insert = tx
        .prepare("INSERT OR REPLACE INTO tracks (track_id, track) VALUES (?, ?)")
        .context("Failed to prepare DuckDB insert statement")?;

    let mut count = 0;

    // Sort tracks by ID for consistent processing
    let mut sorted_tracks: Vec<_> = tracks.into_iter().collect();
    sorted_tracks.sort_by_key(|(track_id, _)| *track_id);

    for (track_id, track_data) in sorted_tracks {
        let track_json = track_data.to_string();

        insert
            .execute(&[&track_id as &dyn duckdb::types::ToSql, &track_json])
            .with_context(|| format!("Failed to insert track {} into DuckDB", track_id))?;

        count += 1;
    }

    tx.commit().context("Failed to commit DuckDB transaction")?;
    Ok(count)
}

fn get_track_id_range(sqlite: &SqliteConn, min_track_id: i64) -> Result<(i64, i64)> {
    let (min_id, max_id): (Option<i64>, Option<i64>) = sqlite
        .query_row(
            "SELECT MIN(track_id), MAX(track_id) FROM tracks WHERE track_id >= ?",
            params![min_track_id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .context("Failed to get track ID range")?;

    match (min_id, max_id) {
        (Some(min), Some(max)) => Ok((min, max)),
        _ => anyhow::bail!("No tracks found with track_id >= {}", min_track_id),
    }
}

fn verify_migration(duck: &DuckConn, expected_count: usize) -> Result<()> {
    let final_count: i64 = duck
        .query_row("SELECT COUNT(*) FROM tracks", [], |row| row.get(0))
        .context("Failed to verify migration count")?;

    if final_count as usize != expected_count {
        log::warn!(
            "Count mismatch: inserted {} tracks but DuckDB shows {}",
            expected_count,
            final_count
        );
    } else {
        log::info!(
            "Migration verification successful: {} tracks in DuckDB",
            final_count
        );
    }

    Ok(())
}

pub fn run_migrate(
    sqlite_path: &str,
    duckdb_path: &str,
    parquet_path: &str,
    min_track_id: i64,
) -> Result<()> {
    // Validate inputs
    if min_track_id < 0 {
        anyhow::bail!("min_track_id must be non-negative, got: {}", min_track_id);
    }

    let start_time = Instant::now();

    // Setup connections
    let sqlite = setup_sqlite_connection(sqlite_path)?;
    let mut duck = setup_duckdb_connection(duckdb_path)?;

    // Get the range of track IDs to process
    let (min_id, max_id) = get_track_id_range(&sqlite, min_track_id)?;
    log::info!(
        "Processing tracks from {} to {} (total range: {})",
        min_id,
        max_id,
        max_id - min_id + 1
    );

    // Process in batches to avoid memory issues
    let batch_size = 10_000i64; // Adjust based on available memory
    let mut total_inserted = 0;
    let mut current_batch_start = min_id;

    while current_batch_start <= max_id {
        let batch_inserted =
            process_tracks_batch(&sqlite, &mut duck, current_batch_start, batch_size)?;
        total_inserted += batch_inserted;

        current_batch_start += batch_size;

        // Progress report
        let progress =
            ((current_batch_start - min_id) as f64 / (max_id - min_id + 1) as f64) * 100.0;
        let elapsed = start_time.elapsed();
        log::info!(
            "Overall progress: {:.1}% | Total inserted: {} | Elapsed: {:.2?}",
            progress.min(100.0),
            total_inserted,
            elapsed
        );
    }

    // Verify the migration
    verify_migration(&duck, total_inserted)?;

    let total_elapsed = start_time.elapsed();
    log::info!(
        "Migration completed successfully! {} tracks migrated in {:.2?} ({:.2} tracks/sec)",
        total_inserted,
        total_elapsed,
        total_inserted as f64 / total_elapsed.as_secs_f64()
    );

    log::info!("Copying to parquet");
    duck.execute(
        &format!(
            "COPY tracks TO '{}' (FORMAT parquet, COMPRESSION zstd, COMPRESSION_LEVEL 5, PARQUET_VERSION v2);",
            &parquet_path
        ),
        [],
    )
    .context("Failed to copy to parquet")?;
    log::info!("Done copying to parquet");

    Ok(())
}
