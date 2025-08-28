#!/usr/bin/env python3
"""
Shared database functions and base manager class for audio database operations.
"""

import sqlite3
import re
import logging
import time
from pathlib import Path
from typing import Optional, List, Tuple, Set, Dict


class AudioDatabaseManager:
    """Base class for audio track database management with shared functionality."""

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

        # Optimize database settings
        self.cur.execute("PRAGMA foreign_keys = ON")
        self.cur.execute("PRAGMA journal_mode = WAL")
        self.cur.execute("PRAGMA synchronous = NORMAL")
        self.cur.execute("PRAGMA cache_size = -64000")  # 64MB cache
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
                album_id INTEGER NOT NULL,
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
            "CREATE INDEX IF NOT EXISTS idx_gdr_track_id ON content_gdr (track_id)",
            "CREATE INDEX IF NOT EXISTS idx_gdr_account_id ON content_gdr (gdr_account_id)",
            "CREATE INDEX IF NOT EXISTS idx_gdr_email ON gdr_accounts (email)",
            "CREATE INDEX IF NOT EXISTS idx_content_cid_track_content ON content_cid (track_id, content_id)",
            "CREATE INDEX IF NOT EXISTS idx_content_gdr_track_content ON content_gdr (track_id, content_id)",
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

    def bulk_update_album_cache(self, albums: List[str], album_cache: Dict):
        """Efficiently update album cache with missing albums using batched queries."""
        missing = [album for album in albums if album not in album_cache]
        if not missing:
            return

        # Process in smaller batches to avoid "Expression tree is too large" error
        batch_size = 1000  # Albums use simple IN clause, can handle more

        for i in range(0, len(missing), batch_size):
            batch = missing[i : i + batch_size]
            placeholders = ",".join("?" * len(batch))
            self.cur.execute(
                f"SELECT album_id, path FROM albums WHERE path IN ({placeholders})",
                batch,
            )
            for album_id, path in self.cur.fetchall():
                album_cache[path] = album_id

    def bulk_update_track_cache(self, tracks: List[Tuple[int, str]], track_cache: Dict):
        """Efficiently update track cache with missing tracks using batched queries."""
        missing = [track for track in tracks if track not in track_cache]
        if not missing:
            return

        # Process in smaller batches to avoid "Expression tree is too large" error
        batch_size = 500  # SQLite can handle this many OR conditions safely

        for i in range(0, len(missing), batch_size):
            batch = missing[i : i + batch_size]

            # Build query with multiple conditions for this batch
            conditions = []
            params = []
            for album_id, track_name in batch:
                conditions.append("(album_id = ? AND track_name = ?)")
                params.extend([album_id, track_name])

            if conditions:
                query = f"SELECT track_id, album_id, track_name FROM tracks WHERE {' OR '.join(conditions)}"
                self.cur.execute(query, params)
                for track_id, album_id, track_name in self.cur.fetchall():
                    track_cache[(album_id, track_name)] = track_id

    def bulk_update_gdr_account_cache(self, emails: List[str], gdr_account_cache: Dict):
        """Efficiently update GDR account cache with missing emails using batched queries."""
        missing = [email for email in emails if email not in gdr_account_cache]
        if not missing:
            return

        # Process in smaller batches to avoid "Expression tree is too large" error
        batch_size = 1000  # Emails use simple IN clause, can handle more

        for i in range(0, len(missing), batch_size):
            batch = missing[i : i + batch_size]
            placeholders = ",".join("?" * len(batch))
            self.cur.execute(
                f"SELECT gdr_account_id, email FROM gdr_accounts WHERE email IN ({placeholders})",
                batch,
            )
            for gdr_account_id, email in self.cur.fetchall():
                gdr_account_cache[email] = gdr_account_id

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

    def insert_albums_batch(self, albums: List[str]):
        """Insert albums in batch and update cache."""
        if not albums:
            return
        self.cur.executemany(
            "INSERT OR IGNORE INTO albums (path) VALUES (?)",
            [(path,) for path in albums],
        )

    def insert_tracks_batch(self, tracks: List[Tuple[int, str]]):
        """Insert tracks in batch."""
        if not tracks:
            return
        self.cur.executemany(
            "INSERT OR IGNORE INTO tracks (album_id, track_name) VALUES (?, ?)",
            tracks,
        )

    def insert_gdr_accounts_batch(self, emails: List[str]):
        """Insert GDR accounts in batch."""
        if not emails:
            return
        self.cur.executemany(
            "INSERT OR IGNORE INTO gdr_accounts (email) VALUES (?)",
            [(email,) for email in emails],
        )

    def insert_content_cid_batch(self, content: List[Tuple[int, str]]) -> int:
        """Insert content CID records in batch."""
        if not content:
            return 0
        self.cur.executemany(
            "INSERT OR IGNORE INTO content_cid (track_id, cid) VALUES (?, ?)",
            content,
        )
        return len(content)

    def insert_content_gdr_batch(self, gdr_content: List[Tuple]) -> int:
        """Insert GDR content records in batch."""
        if not gdr_content:
            return 0
        self.cur.executemany(
            """
            INSERT OR IGNORE INTO content_gdr 
            (track_id, gdr_account_id, cid, start_byte, end_byte) 
            VALUES (?, ?, ?, ?, ?)
        """,
            gdr_content,
        )
        return len(gdr_content)

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
