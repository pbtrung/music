#!/usr/bin/env python3
"""
Audio Database Manager
Manages a SQLite database of audio tracks with content IDs (CIDs).
Supports both full rebuild and incremental updates.
"""

import sys
import sqlite3
import re
import csv
import json
import logging
import time
from pathlib import Path
from typing import Optional, List, Tuple, Set, Dict
import argparse


class AudioDatabaseManager:
    """Manages audio track database with CID mappings."""

    def __init__(self, db_path: str):
        self.db_path = Path(db_path)
        self.conn: Optional[sqlite3.Connection] = None
        self.cur: Optional[sqlite3.Cursor] = None
        self.BATCH_SIZE = 10000

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self.conn:
            self.conn.close()

    def connect(self):
        """Connect to database and set up cursor."""
        logging.info(f"Connecting to database: {self.db_path}")
        self.conn = sqlite3.connect(self.db_path)
        self.cur = self.conn.cursor()

        self.cur.execute("PRAGMA foreign_keys = ON")
        # Write-Ahead Logging
        self.cur.execute("PRAGMA journal_mode = WAL")
        # Faster than FULL
        self.cur.execute("PRAGMA synchronous = NORMAL")
        # 64MB cache
        self.cur.execute("PRAGMA cache_size = -64000")
        # Use memory for temp tables
        self.cur.execute("PRAGMA temp_store = MEMORY")

        if self.db_path.exists():
            file_size = self.db_path.stat().st_size
            logging.info(
                f"Database file size: {file_size:,} bytes ({file_size / 1024 / 1024:.2f} MB)"
            )

    def create_tables(self):
        """Create database tables if they don't exist."""
        logging.info("Creating database tables...")
        start_time = time.time()

        tables = [
            """CREATE TABLE IF NOT EXISTS albums (
                album_id INTEGER PRIMARY KEY AUTOINCREMENT,
                path TEXT UNIQUE NOT NULL
            )""",
            """CREATE TABLE IF NOT EXISTS tracks (
                track_id INTEGER PRIMARY KEY AUTOINCREMENT,
                album_id INTEGER,
                track_name TEXT NOT NULL,
                FOREIGN KEY (album_id) REFERENCES albums (album_id) ON DELETE CASCADE
            )""",
            """CREATE TABLE IF NOT EXISTS content_cid (
                content_id INTEGER PRIMARY KEY AUTOINCREMENT,
                track_id INTEGER NOT NULL,
                cid TEXT NOT NULL,
                FOREIGN KEY (track_id) REFERENCES tracks (track_id) ON DELETE CASCADE
            )""",
            """CREATE TABLE IF NOT EXISTS gdr_accounts (
                gdr_account_id INTEGER PRIMARY KEY AUTOINCREMENT,
                email TEXT UNIQUE NOT NULL
            )""",
            """CREATE TABLE IF NOT EXISTS content_gdr (
                content_id INTEGER PRIMARY KEY AUTOINCREMENT,
                track_id INTEGER NOT NULL,
                gdr_account_id INTEGER NOT NULL,
                cid TEXT NOT NULL,
                start_byte INTEGER NOT NULL,
                end_byte INTEGER NOT NULL,
                CHECK (end_byte >= start_byte),
                FOREIGN KEY (track_id) REFERENCES tracks (track_id) ON DELETE CASCADE,
                FOREIGN KEY (gdr_account_id) REFERENCES gdr_accounts (gdr_account_id) ON DELETE CASCADE
            )""",
        ]

        self.cur.execute("BEGIN")
        for table_sql in tables:
            self.cur.execute(table_sql)
        self.cur.execute("COMMIT")

        elapsed = time.time() - start_time
        logging.info(f"Database tables created in {elapsed:.2f} seconds")

    def create_indexes(self):
        """Create database indexes for performance."""
        logging.info("Creating database indexes...")
        start_time = time.time()

        indexes = [
            "CREATE INDEX IF NOT EXISTS idx_album_path ON albums (path)",
            "CREATE INDEX IF NOT EXISTS idx_album_id ON tracks (album_id)",
            "CREATE INDEX IF NOT EXISTS idx_track_name ON tracks (track_name)",
            "CREATE INDEX IF NOT EXISTS idx_track_id ON content_cid (track_id)",
            "CREATE INDEX IF NOT EXISTS idx_cid ON content_cid (cid)",
            "CREATE INDEX IF NOT EXISTS idx_gdr_track_id ON content_gdr (track_id)",
            "CREATE INDEX IF NOT EXISTS idx_gdr_cid ON content_gdr (cid)",
            "CREATE INDEX IF NOT EXISTS idx_gdr_account_id ON content_gdr (gdr_account_id)",
            "CREATE INDEX IF NOT EXISTS idx_gdr_email ON gdr_accounts (email)",
        ]

        for index_sql in indexes:
            self.cur.execute(index_sql)

        elapsed = time.time() - start_time
        logging.info(f"Database indexes created in {elapsed:.2f} seconds")

    def is_valid_audio_file(self, filename: str) -> bool:
        """Check if filename has valid audio extension."""
        return bool(re.search(r"\.(opus|mp3|m4a|m4b)$", filename, re.IGNORECASE))

    def load_existing_data(self) -> Tuple[Dict, Dict, Dict]:
        """Load existing albums, tracks, and accounts into cache."""
        album_cache = {}
        track_cache = {}
        gdr_account_cache = {}

        self.cur.execute("SELECT album_id, path FROM albums")
        for album_id, path in self.cur.fetchall():
            album_cache[path] = album_id

        self.cur.execute("SELECT track_id, album_id, track_name FROM tracks")
        for track_id, album_id, track_name in self.cur.fetchall():
            track_cache[(album_id, track_name)] = track_id

        self.cur.execute("SELECT gdr_account_id, email FROM gdr_accounts")
        for gdr_account_id, email in self.cur.fetchall():
            gdr_account_cache[email] = gdr_account_id

        return album_cache, track_cache, gdr_account_cache

    def get_existing_cids(self) -> Set[str]:
        """Get all existing CIDs for incremental updates."""
        self.cur.execute("SELECT cid FROM content_cid")
        return {row[0] for row in self.cur.fetchall()}

    def get_existing_gdr_content(self) -> Set[Tuple]:
        """Get all existing GDR content combinations for incremental updates."""
        self.cur.execute(
            """
            SELECT t.album_id, t.track_name, cg.gdr_account_id, cg.cid, cg.start_byte, cg.end_byte
            FROM content_gdr cg
            JOIN tracks t ON cg.track_id = t.track_id
            JOIN albums a ON t.album_id = a.album_id
        """
        )
        return {tuple(row) for row in self.cur.fetchall()}

    def flush_batches(
        self,
        albums_batch,
        tracks_batch,
        gdr_accounts_batch,
        content_batch,
        gdr_content_batch,
        album_cache,
        track_cache,
        gdr_account_cache,
    ) -> int:
        """Flush all pending batches to database and update caches."""
        processed = 0

        # Insert albums
        if albums_batch:
            self.cur.executemany(
                "INSERT OR IGNORE INTO albums (path) VALUES (?)",
                [(path,) for path in albums_batch],
            )
            self._update_album_cache(albums_batch, album_cache)
            albums_batch.clear()

        # Insert GDR accounts
        if gdr_accounts_batch:
            self.cur.executemany(
                "INSERT OR IGNORE INTO gdr_accounts (email) VALUES (?)",
                [(email,) for email in gdr_accounts_batch],
            )
            self._update_gdr_account_cache(gdr_accounts_batch, gdr_account_cache)
            gdr_accounts_batch.clear()

        # Insert tracks
        if tracks_batch:
            self.cur.executemany(
                "INSERT OR IGNORE INTO tracks (album_id, track_name) VALUES (?, ?)",
                tracks_batch,
            )
            self._update_track_cache(tracks_batch, track_cache)
            tracks_batch.clear()

        # Insert content
        if content_batch:
            self.cur.executemany(
                "INSERT OR IGNORE INTO content_cid (track_id, cid) VALUES (?, ?)",
                content_batch,
            )
            processed += len(content_batch)
            content_batch.clear()

        # Insert GDR content
        if gdr_content_batch:
            self.cur.executemany(
                """
                INSERT OR IGNORE INTO content_gdr 
                (track_id, gdr_account_id, cid, start_byte, end_byte) 
                VALUES (?, ?, ?, ?, ?)
            """,
                gdr_content_batch,
            )
            processed += len(gdr_content_batch)
            gdr_content_batch.clear()

        return processed

    def _update_album_cache(self, albums_batch, album_cache):
        """Update album cache with newly inserted albums."""
        for path in albums_batch:
            if path not in album_cache:
                self.cur.execute("SELECT album_id FROM albums WHERE path = ?", (path,))
                row = self.cur.fetchone()
                if row:
                    album_cache[path] = row[0]

    def _update_track_cache(self, tracks_batch, track_cache):
        """Update track cache with newly inserted tracks."""
        for album_id, track_name in tracks_batch:
            cache_key = (album_id, track_name)
            if cache_key not in track_cache:
                self.cur.execute(
                    "SELECT track_id FROM tracks WHERE album_id = ? AND track_name = ?",
                    (album_id, track_name),
                )
                row = self.cur.fetchone()
                if row:
                    track_cache[cache_key] = row[0]

    def _update_gdr_account_cache(self, gdr_accounts_batch, gdr_account_cache):
        """Update GDR account cache with newly inserted accounts."""
        for email in gdr_accounts_batch:
            if email not in gdr_account_cache:
                self.cur.execute(
                    "SELECT gdr_account_id FROM gdr_accounts WHERE email = ?", (email,)
                )
                row = self.cur.fetchone()
                if row:
                    gdr_account_cache[email] = row[0]

    def parse_nft_file(self, nft_path: str, incremental: bool = False) -> int:
        """Parse NFT CSV file and insert records using batch processing."""
        logging.info(
            f"{'Incrementally parsing' if incremental else 'Parsing'} NFT file: {nft_path}"
        )
        start_time = time.time()

        # Initialize counters and caches
        processed = 0
        skipped = 0
        invalid_rows = 0
        non_audio_files = 0
        total_rows = 0

        album_cache, track_cache, _ = self.load_existing_data()
        existing_cids = self.get_existing_cids() if incremental else set()

        # Initialize batches
        albums_batch = []
        tracks_batch = []
        content_batch = []

        self.cur.execute("BEGIN")
        try:
            with open(nft_path, "r") as nft_file:
                nft_reader = csv.reader(nft_file, delimiter=",")

                for row_num, row in enumerate(nft_reader, 1):
                    total_rows += 1

                    if row_num % 20000 == 0:
                        logging.info(f"Processing NFT row {row_num:,}...")
                        processed += self.flush_batches(
                            albums_batch,
                            tracks_batch,
                            [],
                            content_batch,
                            [],
                            album_cache,
                            track_cache,
                            {},
                        )

                    if len(row) < 3:
                        logging.warning(f"Skipping invalid row {row_num}: {row}")
                        invalid_rows += 1
                        continue

                    cid = row[0].strip()
                    path = Path(row[2].strip())
                    album_path = str(path.parent)
                    track_name = str(path.name)

                    if not self.is_valid_audio_file(track_name):
                        non_audio_files += 1
                        continue

                    if incremental and cid in existing_cids:
                        skipped += 1
                        continue

                    album_id = self._ensure_album_id(
                        album_path, albums_batch, album_cache
                    )
                    track_id = self._ensure_track_id(
                        album_id, track_name, tracks_batch, track_cache
                    )

                    content_batch.append((track_id, cid))

                    if len(content_batch) >= self.BATCH_SIZE:
                        processed += self.flush_batches(
                            albums_batch,
                            tracks_batch,
                            [],
                            content_batch,
                            [],
                            album_cache,
                            track_cache,
                            {},
                        )

            # Final flush
            processed += self.flush_batches(
                albums_batch,
                tracks_batch,
                [],
                content_batch,
                [],
                album_cache,
                track_cache,
                {},
            )
            self.cur.execute("COMMIT")

        except Exception as e:
            self.cur.execute("ROLLBACK")
            logging.error(f"Error processing NFT file: {e}")
            raise

        self._log_processing_stats(
            "NFT",
            start_time,
            total_rows,
            processed,
            skipped,
            invalid_rows,
            non_audio_files,
        )
        return processed

    def parse_arw_file(self, arw_path: str, incremental: bool = False) -> int:
        """Parse ARW file and insert records using batch processing."""
        logging.info(
            f"{'Incrementally parsing' if incremental else 'Parsing'} ARW file: {arw_path}"
        )
        start_time = time.time()

        # Initialize counters and caches
        processed = 0
        skipped = 0
        invalid_lines = 0
        invalid_audio_files = 0
        total_lines = 0

        album_cache, track_cache, _ = self.load_existing_data()
        existing_cids = self.get_existing_cids() if incremental else set()

        # Initialize batches
        albums_batch = []
        tracks_batch = []
        content_batch = []

        self.cur.execute("BEGIN")
        try:
            with open(arw_path, "r") as arw_file:
                for line_num, line in enumerate(arw_file, 1):
                    total_lines += 1

                    if line_num % 20000 == 0:
                        logging.info(f"Processing ARW line {line_num:,}...")
                        processed += self.flush_batches(
                            albums_batch,
                            tracks_batch,
                            [],
                            content_batch,
                            [],
                            album_cache,
                            track_cache,
                            {},
                        )

                    line = line.strip()
                    if not line:
                        continue

                    parts = line.split(",", 1)
                    if len(parts) < 2:
                        invalid_lines += 1
                        continue

                    cid = parts[0].strip()
                    path = Path(parts[1].strip())

                    if incremental and cid in existing_cids:
                        skipped += 1
                        continue

                    album_path = str(path.parent)
                    if album_path.startswith("data/"):
                        album_path = album_path[5:]

                    filename = str(path.name)
                    match = re.search(
                        r"(.*)\.(opus|mp3|m4a|m4b)[.]?(\d*)$", filename, re.IGNORECASE
                    )
                    if not match:
                        invalid_audio_files += 1
                        continue

                    orig_filename = f"{match.group(1)}.{match.group(2)}"

                    album_id = self._ensure_album_id(
                        album_path, albums_batch, album_cache
                    )
                    track_id = self._ensure_track_id(
                        album_id, orig_filename, tracks_batch, track_cache
                    )

                    content_batch.append((track_id, cid))

                    if len(content_batch) >= self.BATCH_SIZE:
                        processed += self.flush_batches(
                            albums_batch,
                            tracks_batch,
                            [],
                            content_batch,
                            [],
                            album_cache,
                            track_cache,
                            {},
                        )

            # Final flush
            processed += self.flush_batches(
                albums_batch,
                tracks_batch,
                [],
                content_batch,
                [],
                album_cache,
                track_cache,
                {},
            )
            self.cur.execute("COMMIT")

        except Exception as e:
            self.cur.execute("ROLLBACK")
            logging.error(f"Error processing ARW file: {e}")
            raise

        self._log_arw_stats(
            start_time,
            total_lines,
            processed,
            skipped,
            invalid_lines,
            invalid_audio_files,
        )
        return processed

    def parse_json_files(self, json_files: List[str], incremental: bool = False) -> int:
        """Parse JSON files and insert GDR records using batch processing."""
        logging.info(
            f"{'Incrementally parsing' if incremental else 'Parsing'} {len(json_files)} JSON files"
        )
        start_time = time.time()

        # Initialize counters and caches
        processed = 0
        skipped = 0
        invalid_records = 0
        invalid_files = 0
        non_audio_files = 0
        total_records = 0
        total_files_in_records = 0

        album_cache, track_cache, gdr_account_cache = self.load_existing_data()
        existing_gdr_content = self.get_existing_gdr_content() if incremental else set()

        # Initialize batches
        albums_batch = []
        tracks_batch = []
        gdr_accounts_batch = []
        gdr_content_batch = []

        self.cur.execute("BEGIN")
        try:
            for file_num, json_file_path in enumerate(json_files, 1):
                logging.info(
                    f"Processing JSON file {file_num}/{len(json_files)}: {json_file_path}"
                )

                with open(json_file_path, "r", encoding="utf-8") as json_file:
                    try:
                        data = json.load(json_file)
                    except json.JSONDecodeError as e:
                        logging.error(f"Invalid JSON in {json_file_path}: {e}")
                        continue

                for record_num, record in enumerate(data, 1):
                    total_records += 1

                    if record_num % 1000 == 0:
                        processed += self.flush_batches(
                            albums_batch,
                            tracks_batch,
                            gdr_accounts_batch,
                            [],
                            gdr_content_batch,
                            album_cache,
                            track_cache,
                            gdr_account_cache,
                        )

                    if not self._is_valid_json_record(record):
                        invalid_records += 1
                        continue

                    result = self._process_json_record(
                        record,
                        albums_batch,
                        tracks_batch,
                        gdr_accounts_batch,
                        gdr_content_batch,
                        album_cache,
                        track_cache,
                        gdr_account_cache,
                        existing_gdr_content,
                        incremental,
                    )

                    if result == "skipped":
                        skipped += 1
                    elif result == "invalid_file":
                        invalid_files += 1
                    elif result == "non_audio":
                        non_audio_files += 1

                    total_files_in_records += 1

                    if len(gdr_content_batch) >= self.BATCH_SIZE:
                        processed += self.flush_batches(
                            albums_batch,
                            tracks_batch,
                            gdr_accounts_batch,
                            [],
                            gdr_content_batch,
                            album_cache,
                            track_cache,
                            gdr_account_cache,
                        )

            # Final flush
            processed += self.flush_batches(
                albums_batch,
                tracks_batch,
                gdr_accounts_batch,
                [],
                gdr_content_batch,
                album_cache,
                track_cache,
                gdr_account_cache,
            )
            self.cur.execute("COMMIT")

        except Exception as e:
            self.cur.execute("ROLLBACK")
            logging.error(f"Error processing JSON files: {e}")
            raise

        self._log_json_stats(
            start_time,
            len(json_files),
            total_records,
            total_files_in_records,
            processed,
            skipped,
            invalid_records,
            invalid_files,
            non_audio_files,
        )
        return processed

    def _ensure_album_id(
        self, album_path: str, albums_batch: List, album_cache: Dict
    ) -> int:
        """Ensure album exists and return its ID."""
        if album_path not in album_cache:
            if album_path not in albums_batch:
                albums_batch.append(album_path)
            return None  # Will be resolved after batch flush
        return album_cache[album_path]

    def _ensure_track_id(
        self, album_id: int, track_name: str, tracks_batch: List, track_cache: Dict
    ) -> int:
        """Ensure track exists and return its ID."""
        cache_key = (album_id, track_name)
        if cache_key not in track_cache:
            if (album_id, track_name) not in tracks_batch:
                tracks_batch.append((album_id, track_name))
            return None  # Will be resolved after batch flush
        return track_cache[cache_key]

    def _is_valid_json_record(self, record) -> bool:
        """Check if JSON record has required fields."""
        if not isinstance(record, dict):
            return False

        subfolder = record.get("subfolder", "").strip()
        merged_file = record.get("merged_file", {})
        files = record.get("files", [])

        return bool(subfolder and merged_file and files)

    def _process_json_record(
        self,
        record,
        albums_batch,
        tracks_batch,
        gdr_accounts_batch,
        gdr_content_batch,
        album_cache,
        track_cache,
        gdr_account_cache,
        existing_gdr_content,
        incremental,
    ) -> str:
        """Process a single JSON record. Returns status: 'processed', 'skipped', 'invalid_file', or 'non_audio'."""
        subfolder = record["subfolder"].strip()
        merged_file = record["merged_file"]
        files = record["files"]

        file_id = merged_file.get("file_id", "").strip()
        email = merged_file.get("email", "").strip()

        if not file_id or not email:
            return "invalid_file"

        # Ensure GDR account and album exist
        if email not in gdr_account_cache and email not in gdr_accounts_batch:
            gdr_accounts_batch.append(email)

        if subfolder not in album_cache and subfolder not in albums_batch:
            albums_batch.append(subfolder)

        for file_info in files:
            if not isinstance(file_info, dict):
                return "invalid_file"

            file_name = file_info.get("file_name", "").strip()
            byte_range = file_info.get("byte_range", [])

            if not file_name or len(byte_range) != 2:
                return "invalid_file"

            if not self.is_valid_audio_file(file_name):
                return "non_audio"

            start_byte, end_byte = byte_range[0], byte_range[1]

            # Check for existing content in incremental mode
            if incremental:
                album_id = album_cache.get(subfolder)
                gdr_account_id = gdr_account_cache.get(email)
                if album_id and gdr_account_id:
                    content_key = (
                        album_id,
                        file_name,
                        gdr_account_id,
                        file_id,
                        start_byte,
                        end_byte,
                    )
                    if content_key in existing_gdr_content:
                        return "skipped"

            # Ensure track exists
            album_id = album_cache.get(subfolder)
            if not album_id:
                return "processed"  # Will be handled in batch processing

            track_cache_key = (album_id, file_name)
            if track_cache_key not in track_cache:
                if (album_id, file_name) not in tracks_batch:
                    tracks_batch.append((album_id, file_name))

            # Add to GDR content batch - IDs will be resolved during flush
            gdr_content_batch.append(
                (None, None, file_id, start_byte, end_byte, subfolder, file_name, email)
            )

        return "processed"

    def _log_processing_stats(
        self,
        file_type: str,
        start_time: float,
        total_rows: int,
        processed: int,
        skipped: int,
        invalid_rows: int,
        non_audio_files: int,
    ):
        """Log processing statistics."""
        elapsed = time.time() - start_time
        logging.info(f"{file_type} file processing complete:")
        logging.info(f"  - Total rows processed: {total_rows:,}")
        logging.info(f"  - Records added: {processed:,}")
        logging.info(f"  - Records skipped: {skipped:,}")
        logging.info(f"  - Invalid rows: {invalid_rows:,}")
        logging.info(f"  - Non-audio files: {non_audio_files:,}")
        logging.info(f"  - Processing time: {elapsed:.2f} seconds")
        logging.info(f"  - Processing rate: {total_rows / elapsed:.0f} rows/second")

    def _log_arw_stats(
        self,
        start_time: float,
        total_lines: int,
        processed: int,
        skipped: int,
        invalid_lines: int,
        invalid_audio_files: int,
    ):
        """Log ARW processing statistics."""
        elapsed = time.time() - start_time
        logging.info(f"ARW file processing complete:")
        logging.info(f"  - Total lines processed: {total_lines:,}")
        logging.info(f"  - Records added: {processed:,}")
        logging.info(f"  - Records skipped: {skipped:,}")
        logging.info(f"  - Invalid lines: {invalid_lines:,}")
        logging.info(f"  - Invalid audio files: {invalid_audio_files:,}")
        logging.info(f"  - Processing time: {elapsed:.2f} seconds")
        logging.info(f"  - Processing rate: {total_lines / elapsed:.0f} lines/second")

    def _log_json_stats(
        self,
        start_time: float,
        file_count: int,
        total_records: int,
        total_files: int,
        processed: int,
        skipped: int,
        invalid_records: int,
        invalid_files: int,
        non_audio_files: int,
    ):
        """Log JSON processing statistics."""
        elapsed = time.time() - start_time
        logging.info(f"JSON files processing complete:")
        logging.info(f"  - Total JSON files: {file_count}")
        logging.info(f"  - Total records processed: {total_records:,}")
        logging.info(f"  - Total files in records: {total_files:,}")
        logging.info(f"  - GDR content added: {processed:,}")
        logging.info(f"  - GDR content skipped: {skipped:,}")
        logging.info(f"  - Invalid records: {invalid_records:,}")
        logging.info(f"  - Invalid files: {invalid_files:,}")
        logging.info(f"  - Non-audio files: {non_audio_files:,}")
        logging.info(f"  - Processing time: {elapsed:.2f} seconds")

    def vacuum(self):
        """Optimize database by running VACUUM."""
        logging.info("Running VACUUM to optimize database...")
        start_time = time.time()

        file_size_before = self.db_path.stat().st_size if self.db_path.exists() else 0
        self.conn.execute("VACUUM")
        self.conn.commit()
        file_size_after = self.db_path.stat().st_size if self.db_path.exists() else 0

        elapsed = time.time() - start_time
        size_diff = file_size_before - file_size_after

        logging.info(f"VACUUM completed in {elapsed:.2f} seconds")
        logging.info(f"Database size: {file_size_before:,} → {file_size_after:,} bytes")
        if size_diff != 0:
            logging.info(
                f"Size change: {size_diff:,} bytes ({size_diff / 1024 / 1024:.2f} MB)"
            )

    def get_stats(self) -> Dict:
        """Get database statistics."""
        stats = {}
        queries = [
            ("albums", "SELECT COUNT(*) FROM albums"),
            ("tracks", "SELECT COUNT(*) FROM tracks"),
            ("content_cid", "SELECT COUNT(*) FROM content_cid"),
            ("gdr_accounts", "SELECT COUNT(*) FROM gdr_accounts"),
            ("content_gdr", "SELECT COUNT(*) FROM content_gdr"),
        ]

        for name, query in queries:
            self.cur.execute(query)
            stats[name] = self.cur.fetchone()[0]

        if self.db_path.exists():
            file_size = self.db_path.stat().st_size
            stats["file_size_bytes"] = file_size
            stats["file_size_mb"] = file_size / 1024 / 1024

        return stats


def setup_logging(verbose: bool = False):
    """Set up logging configuration."""
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s - %(levelname)s - %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Audio Database Manager - Manage SQLite database of audio tracks with CIDs"
    )

    parser.add_argument("--nft-file", help="Path to NFT CSV file", default=None)
    parser.add_argument("--arw-file", help="Path to ARW file", default=None)
    parser.add_argument(
        "--json-files", help="Comma-separated list of JSON files", default=""
    )
    parser.add_argument("database", help="Path to SQLite database file")
    parser.add_argument(
        "-i",
        "--incremental",
        action="store_true",
        help="Perform incremental update (skip existing CIDs)",
    )
    parser.add_argument(
        "--no-vacuum", action="store_true", help="Skip database VACUUM optimization"
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Enable verbose logging"
    )
    parser.add_argument(
        "--rebuild",
        action="store_true",
        help="Force rebuild by deleting existing database",
    )

    args = parser.parse_args()
    setup_logging(args.verbose)

    # Validate input files
    if args.nft_file and not Path(args.nft_file).exists():
        logging.error(f"NFT file not found: {args.nft_file}")
        sys.exit(1)

    if args.arw_file and not Path(args.arw_file).exists():
        logging.error(f"ARW file not found: {args.arw_file}")
        sys.exit(1)

    json_file_list = [f.strip() for f in args.json_files.split(",") if f.strip()]
    for json_file in json_file_list:
        if not Path(json_file).exists():
            logging.error(f"JSON file not found: {json_file}")
            sys.exit(1)

    # Handle database creation/rebuild
    db_path = Path(args.database)
    if args.rebuild and db_path.exists():
        logging.info(f"Rebuilding database: {db_path}")
        db_path.unlink()

    # Process files
    try:
        with AudioDatabaseManager(str(db_path)) as db_manager:
            db_manager.create_tables()

            if args.incremental and db_path.exists():
                initial_stats = db_manager.get_stats()
                logging.info(f"Initial database stats: {initial_stats}")

            nft_processed = arw_processed = json_processed = 0

            if args.nft_file:
                nft_processed = db_manager.parse_nft_file(
                    args.nft_file, args.incremental
                )

            if args.arw_file:
                arw_processed = db_manager.parse_arw_file(
                    args.arw_file, args.incremental
                )

            if json_file_list:
                json_processed = db_manager.parse_json_files(
                    json_file_list, args.incremental
                )

            db_manager.create_indexes()

            if not args.no_vacuum:
                db_manager.vacuum()

            final_stats = db_manager.get_stats()
            logging.info(f"Final database stats: {final_stats}")
            logging.info(
                f"Total records processed: NFT={nft_processed}, ARW={arw_processed}, JSON={json_processed}"
            )

    except Exception as e:
        logging.error(f"Database operation failed: {e}")
        sys.exit(1)

    logging.info("Database operation completed successfully")


if __name__ == "__main__":
    """
    # Full rebuild with all sources
    python sqlite.py --nft-file nft.csv --arw-file arw.csv --json-files "001.json,002.json,003.json" data.db --rebuild

    # Incremental update with NFT + ARW
    python sqlite.py --nft-file nft.csv --arw-file arw.csv --json-files "001.json,002.json" data.db --incremental

    # Incremental update with only NFT
    python sqlite.py --nft-file nft.csv data.db --incremental

    # Incremental update with only ARW
    python sqlite.py --arw-file arw.csv data.db --incremental

    # Incremental update with only JSON
    python sqlite.py --json-files "001.json" data.db --incremental

    # Verbose logging
    python sqlite.py --nft-file nft.csv --arw-file arw.csv --json-files "001.json" data.db --incremental --verbose

    # Skip vacuum optimization
    python sqlite.py --nft-file nft.csv --arw-file arw.csv --json-files "001.json" data.db --no-vacuum

    # Without JSON files (empty list, still valid)
    python sqlite.py --nft-file nft.csv --arw-file arw.csv --json-files "" data.db --incremental
    """

    main()
