use anyhow::Result;
use duckdb::Connection as DuckConn;
use rusqlite::{Connection as SqliteConn, params};
use serde_json::{Value, json};
use std::time::Instant;

pub fn run_migrate(sqlite_path: &str, duckdb_path: &str, min_track_id: i64) -> Result<()> {
    // open connections
    let sqlite = SqliteConn::open(sqlite_path)?;
    let mut duck = DuckConn::open(duckdb_path)?;

    // create table in DuckDB
    duck.execute(
        "CREATE TABLE IF NOT EXISTS tracks (
            track_id INTEGER PRIMARY KEY,
            track JSON
        )",
        [],
    )?;

    // optimize SQLite for faster reads
    sqlite.execute_batch(
        "
        PRAGMA synchronous = OFF;
        PRAGMA journal_mode = MEMORY;
        PRAGMA temp_store = MEMORY;
        ",
    )?;

    // prepare SQLite query
    let mut stmt = sqlite.prepare(
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
    )?;

    let mut rows = stmt.query(params![min_track_id])?;

    // prepare DuckDB insert
    let mut tx = duck.transaction()?;
    let mut insert = tx.prepare("INSERT OR REPLACE INTO tracks (track_id, track) VALUES (?, ?)")?;

    let mut current_id: Option<i64> = None;
    let mut current_track: Option<Value> = None;

    let mut count = 0;
    let chunk_size = 50_000;
    let log_interval = 5_000;
    let mut last_logged_id: Option<i64> = None;

    let start_time = Instant::now();
    let mut last_log_time = Instant::now();

    while let Some(row) = rows.next()? {
        let track_id: i64 = row.get(0)?;
        let track_name: String = row.get(1)?;
        let album_id: i64 = row.get(2)?;
        let path: String = row.get(3)?;
        let content_cid: Option<String> = row.get(4)?;
        let start_byte: Option<i64> = row.get(5)?;
        let end_byte: Option<i64> = row.get(6)?;
        let gdr_account_id: Option<i64> = row.get(7)?;
        let email: Option<String> = row.get(8)?;

        // if we moved to a new track_id, flush the previous one
        if let Some(curr_id) = current_id {
            if track_id != curr_id {
                let track_json = current_track.unwrap().to_string();
                insert.execute(&[&curr_id as &dyn duckdb::types::ToSql, &track_json])?;
                count += 1;

                // log progress
                if count % log_interval == 0 {
                    let interval = last_log_time.elapsed();
                    let total = start_time.elapsed();
                    log::info!(
                        "Processed {} tracks so far (track_id {} to {}) | Interval: {:.2?}, Total: {:.2?}",
                        count,
                        last_logged_id.map_or(curr_id, |v| v + 1),
                        curr_id,
                        interval,
                        total
                    );
                    last_logged_id = Some(curr_id);
                    last_log_time = Instant::now();
                }

                // commit chunk
                if count % chunk_size == 0 {
                    tx.commit()?;
                    let interval = last_log_time.elapsed();
                    let total = start_time.elapsed();
                    log::info!(
                        "Committed {} tracks (track_id {} to {}) | Interval: {:.2?}, Total: {:.2?}",
                        count,
                        last_logged_id.map_or(curr_id, |v| v + 1),
                        curr_id,
                        interval,
                        total
                    );
                    tx = duck.transaction()?;
                    insert = tx
                        .prepare("INSERT OR REPLACE INTO tracks (track_id, track) VALUES (?, ?)")?;
                    last_logged_id = Some(curr_id);
                    last_log_time = Instant::now(); // reset interval timer
                }

                current_track = None;
            }
        }

        // initialize track JSON if needed
        if current_track.is_none() {
            current_id = Some(track_id);
            current_track = Some(json!({
                "track_id": track_id,
                "track_name": track_name,
                "album": {
                    "album_id": album_id,
                    "path": path,
                },
                "cids": []
            }));
        }

        let entry = current_track.as_mut().unwrap();

        // add cid
        if let Some(cid) = content_cid {
            entry["cids"].as_array_mut().unwrap().push(json!(cid));
        }

        // overwrite 1:1 fields
        if let (Some(start), Some(end)) = (start_byte, end_byte) {
            entry["byte_range"] = json!([start as u64, end as u64]);
        }
        if let Some(acc) = gdr_account_id {
            entry["gdr_account_id"] = json!(acc);
        }
        if let Some(mail) = email {
            entry["email"] = json!(mail);
        }
    }

    // flush last track
    if let Some(curr_id) = current_id {
        let track_json = current_track.unwrap().to_string();
        insert.execute(&[&curr_id as &dyn duckdb::types::ToSql, &track_json])?;
        count += 1;

        let interval = last_log_time.elapsed();
        let total = start_time.elapsed();
        log::info!(
            "Final flush: processed {} tracks (track_id {} to {}) | Interval: {:.2?}, Total: {:.2?}",
            count,
            last_logged_id.map_or(min_track_id, |v| v + 1),
            curr_id,
            interval,
            total
        );
    }

    // commit remaining tracks
    tx.commit()?;
    let total_elapsed = start_time.elapsed();
    log::info!(
        "Migration finished. Total tracks migrated: {} | Total elapsed: {:.2?}",
        count,
        total_elapsed
    );

    Ok(())
}
