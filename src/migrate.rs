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
            start_byte: row.get(5).ok(),
            end_byte: row.get(6).ok(),
            gdr_account_id: row.get(7).ok(),
            email: row.get(8).ok(),
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

fn read_tracks_from_sqlite(sqlite: &SqliteConn, min_track_id: i64) -> Result<HashMap<i64, Value>> {
    let mut stmt = sqlite
        .prepare(
            r#"
        SELECT 
            t.track_id, 
            t.track_name, 
            a.album_id, 
            a.path,
            cc.cid as content_cid,
            cg.start_byte,
            cg.end_byte,
            cg.gdr_account_id,
            ga.email
        FROM tracks t
        JOIN albums a ON t.album_id = a.album_id
        LEFT JOIN content_cid cc ON t.track_id = cc.track_id
        LEFT JOIN content_gdr cg ON t.track_id = cg.track_id
        LEFT JOIN gdr_accounts ga ON cg.gdr_account_id = ga.gdr_account_id
        WHERE t.track_id >= ?
        ORDER BY t.track_id, cc.content_id, cg.content_id
        "#,
        )
        .context("Failed to prepare SQLite query")?;

    let mut rows = stmt
        .query(params![min_track_id])
        .context("Failed to execute SQLite query")?;

    let mut tracks: HashMap<i64, Value> = HashMap::new();
    let mut row_count = 0;

    while let Some(row) = rows.next()? {
        let track_row = TrackRow::from_row(row)?;

        if !track_row.validate() {
            continue;
        }

        process_track_row(&mut tracks, track_row)?;
        row_count += 1;

        if row_count % 10000 == 0 {
            log::info!(
                "Read {} rows from SQLite, {} unique tracks so far",
                row_count,
                tracks.len()
            );
        }
    }

    log::info!(
        "Finished reading SQLite data. Total rows: {}, unique tracks: {}",
        row_count,
        tracks.len()
    );
    Ok(tracks)
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

    // Handle CIDs (1-to-many relationship)
    add_cid_to_track(track_entry, row.content_cid)?;

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

fn add_cid_to_track(track_entry: &mut Value, content_cid: Option<String>) -> Result<()> {
    if let Some(cid) = content_cid {
        if !cid.trim().is_empty() {
            let cids = track_entry["cids"]
                .as_array_mut()
                .context("cids field is not an array")?;
            let cid_value = json!(cid.trim());
            if !cids.contains(&cid_value) {
                cids.push(cid_value);
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

fn insert_tracks_to_duckdb(duck: &mut DuckConn, tracks: HashMap<i64, Value>) -> Result<usize> {
    let chunk_size = 50_000;
    let mut tx = duck
        .transaction()
        .context("Failed to start DuckDB transaction")?;

    let mut insert = tx
        .prepare("INSERT OR REPLACE INTO tracks (track_id, track) VALUES (?, ?)")
        .context("Failed to prepare DuckDB insert statement")?;

    let start_time = Instant::now();
    let mut count = 0;
    let total_tracks = tracks.len();

    // Sort tracks by ID for consistent processing
    let mut sorted_tracks: Vec<_> = tracks.into_iter().collect();
    sorted_tracks.sort_by_key(|(track_id, _)| *track_id);

    for (track_id, track_data) in sorted_tracks {
        let track_json = track_data.to_string();

        insert
            .execute(&[&track_id as &dyn duckdb::types::ToSql, &track_json])
            .with_context(|| format!("Failed to insert track {} into DuckDB", track_id))?;

        count += 1;

        // Log progress
        if count % 5000 == 0 {
            let progress = (count as f64 / total_tracks as f64) * 100.0;
            let elapsed = start_time.elapsed();
            log::info!(
                "Progress: {}/{} ({:.1}%) tracks inserted | Track ID: {} | Elapsed: {:.2?}",
                count,
                total_tracks,
                progress,
                track_id,
                elapsed
            );
        }

        // Commit in chunks to avoid memory issues
        if count % chunk_size == 0 {
            tx.commit().context("Failed to commit DuckDB transaction")?;
            log::info!(
                "Committed chunk of {} tracks (up to track_id: {})",
                chunk_size,
                track_id
            );

            // Start new transaction
            tx = duck
                .transaction()
                .context("Failed to start new DuckDB transaction")?;
            insert = tx
                .prepare("INSERT OR REPLACE INTO tracks (track_id, track) VALUES (?, ?)")
                .context("Failed to prepare new DuckDB insert statement")?;
        }
    }

    // Commit remaining tracks
    tx.commit()
        .context("Failed to commit final DuckDB transaction")?;

    let total_elapsed = start_time.elapsed();
    log::info!(
        "Migration completed successfully! {} tracks migrated in {:.2?} ({:.2} tracks/sec)",
        count,
        total_elapsed,
        count as f64 / total_elapsed.as_secs_f64()
    );

    Ok(count)
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

pub fn run_migrate(sqlite_path: &str, duckdb_path: &str, min_track_id: i64) -> Result<()> {
    // Validate inputs
    if min_track_id < 0 {
        anyhow::bail!("min_track_id must be non-negative, got: {}", min_track_id);
    }

    // Setup connections
    let sqlite = setup_sqlite_connection(sqlite_path)?;
    let mut duck = setup_duckdb_connection(duckdb_path)?;

    // Read all tracks from SQLite
    let tracks = read_tracks_from_sqlite(&sqlite, min_track_id)?;

    // Insert tracks into DuckDB
    let count = insert_tracks_to_duckdb(&mut duck, tracks)?;

    // Verify the migration
    verify_migration(&duck, count)?;

    Ok(())
}
