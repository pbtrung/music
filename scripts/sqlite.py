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
from pathlib import Path
from typing import Optional
import argparse


class AudioDatabaseManager:
    """Manages audio track database with CID mappings."""

    def __init__(self, db_path: str):
        self.db_path = Path(db_path)
        self.conn: Optional[sqlite3.Connection] = None
        self.cur: Optional[sqlite3.Cursor] = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self.conn:
            self.conn.close()

    def connect(self):
        """Connect to database and set up cursor."""
        self.conn = sqlite3.connect(self.db_path)
        self.cur = self.conn.cursor()
        self.cur.execute("PRAGMA foreign_keys = ON")

    def create_tables(self):
        """Create database tables if they don't exist."""
        self.cur.execute("BEGIN")

        # Albums table
        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS albums (
                album_id INTEGER PRIMARY KEY AUTOINCREMENT,
                path TEXT UNIQUE NOT NULL
            )
        """
        )

        # Tracks table
        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS tracks (
                track_id INTEGER PRIMARY KEY AUTOINCREMENT,
                album_id INTEGER,
                track_name TEXT NOT NULL,
                FOREIGN KEY (album_id) REFERENCES albums (album_id) ON DELETE CASCADE
            )
        """
        )

        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS content_cid (
                content_id INTEGER PRIMARY KEY AUTOINCREMENT,
                track_id INTEGER NOT NULL,
                cid TEXT NOT NULL,
                FOREIGN KEY (track_id) REFERENCES tracks (track_id) ON DELETE CASCADE
            )
        """
        )

        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS gdr_accounts (
                gdr_account_id INTEGER PRIMARY KEY AUTOINCREMENT,
                email TEXT UNIQUE NOT NULL
            )
        """
        )

        self.cur.execute(
            """
            CREATE TABLE IF NOT EXISTS content_gdr (
                content_id INTEGER PRIMARY KEY AUTOINCREMENT,
                track_id INTEGER NOT NULL,
                gdr_account_id INTEGER NOT NULL,
                cid TEXT NOT NULL,
                start_byte INTEGER NOT NULL,
                end_byte INTEGER NOT NULL,
                CHECK (end_byte >= start_byte),
                FOREIGN KEY (track_id) REFERENCES tracks (track_id) ON DELETE CASCADE,
                FOREIGN KEY (gdr_account_id) REFERENCES gdr_accounts (gdr_account_id) ON DELETE CASCADE
            )
        """
        )

        self.cur.execute("COMMIT")

    def create_indexes(self):
        """Create database indexes for performance."""
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

    def get_or_create_album(self, album_path: str) -> int:
        """Get existing album ID or create new album."""
        self.cur.execute("SELECT album_id FROM albums WHERE path = ?", (album_path,))
        row = self.cur.fetchone()

        if row:
            return row[0]

        self.cur.execute("INSERT INTO albums (path) VALUES (?)", (album_path,))
        return self.cur.lastrowid

    def get_or_create_track(self, album_id: int, track_name: str) -> int:
        """Get existing track ID or create new track."""
        self.cur.execute(
            "SELECT track_id FROM tracks WHERE album_id = ? AND track_name = ?",
            (album_id, track_name),
        )
        row = self.cur.fetchone()

        if row:
            return row[0]

        self.cur.execute(
            "INSERT INTO tracks (album_id, track_name) VALUES (?, ?)",
            (album_id, track_name),
        )
        return self.cur.lastrowid

    def add_content(self, track_id: int, cid: str) -> bool:
        """Add content CID for a track. Returns True if added, False if exists."""
        try:
            self.cur.execute(
                "INSERT INTO content_cid (track_id, cid) VALUES (?, ?)", (track_id, cid)
            )
            return True
        except sqlite3.IntegrityError:
            # CID already exists, skip
            logging.debug(f"CID {cid} already exists for track {track_id}")
            return False
        except sqlite3.Error as error:
            logging.error(f"Error inserting content: {error}")
            logging.error(f"track_id: {track_id}, cid: {cid}")
            raise

    def is_valid_audio_file(self, filename: str) -> bool:
        """Check if filename has valid audio extension."""
        return bool(re.search(r"\.(opus|mp3|m4a|m4b)$", filename, re.IGNORECASE))

    def parse_nft_file(self, nft_path: str, incremental: bool = False) -> int:
        """
        Parse NFT CSV file and insert records.
        Returns number of records processed.
        """
        processed = 0
        skipped = 0

        self.cur.execute("BEGIN")

        try:
            with open(nft_path, "r") as nft_file:
                nft_reader = csv.reader(nft_file, delimiter=",")

                for row in nft_reader:
                    if len(row) < 3:
                        logging.warning(f"Skipping invalid row: {row}")
                        continue

                    cid = row[0].strip()
                    path = Path(row[2].strip())
                    album_path = str(path.parent)
                    track_name = str(path.name)

                    if not self.is_valid_audio_file(track_name):
                        logging.warning(f"Skipping non-audio file: {track_name}")
                        continue

                    # Check if CID already exists when doing incremental
                    if incremental:
                        self.cur.execute(
                            "SELECT 1 FROM content_cid WHERE cid = ?", (cid,)
                        )
                        if self.cur.fetchone():
                            skipped += 1
                            continue

                    album_id = self.get_or_create_album(album_path)
                    track_id = self.get_or_create_track(album_id, track_name)

                    if self.add_content(track_id, cid):
                        processed += 1
                    else:
                        skipped += 1

            self.cur.execute("COMMIT")
            logging.info(f"NFT file processed: {processed} added, {skipped} skipped")

        except Exception as e:
            self.cur.execute("ROLLBACK")
            logging.error(f"Error processing NFT file: {e}")
            raise

        return processed

    def get_or_create_gdr_account(self, email: str) -> int:
        """Get existing GDR account ID or create new account."""
        self.cur.execute(
            "SELECT gdr_account_id FROM gdr_accounts WHERE email = ?", (email,)
        )
        row = self.cur.fetchone()

        if row:
            return row[0]

        self.cur.execute("INSERT INTO gdr_accounts (email) VALUES (?)", (email,))
        return self.cur.lastrowid

    def add_gdr_content(
        self,
        track_id: int,
        gdr_account_id: int,
        cid: str,
        start_byte: int,
        end_byte: int,
    ) -> bool:
        """Add GDR content for a track. Returns True if added, False if exists."""
        try:
            self.cur.execute(
                "INSERT INTO content_gdr (track_id, gdr_account_id, cid, start_byte, end_byte) VALUES (?, ?, ?, ?, ?)",
                (track_id, gdr_account_id, cid, start_byte, end_byte),
            )
            return True
        except sqlite3.IntegrityError:
            # Content already exists, skip
            logging.debug(f"GDR content already exists for track {track_id}, CID {cid}")
            return False
        except sqlite3.Error as error:
            logging.error(f"Error inserting GDR content: {error}")
            logging.error(
                f"track_id: {track_id}, gdr_account_id: {gdr_account_id}, cid: {cid}"
            )
            raise

    def parse_json_files(self, json_files: list, incremental: bool = False) -> int:
        """
        Parse JSON files and insert GDR records.
        Returns number of records processed.
        """
        processed = 0
        skipped = 0

        self.cur.execute("BEGIN")

        try:
            for json_file_path in json_files:
                if not Path(json_file_path).exists():
                    logging.warning(f"JSON file not found: {json_file_path}")
                    continue

                logging.info(f"Processing JSON file: {json_file_path}")

                with open(json_file_path, "r", encoding="utf-8") as json_file:
                    data = json.load(json_file)

                for record in data:
                    if not isinstance(record, dict):
                        logging.warning(f"Skipping invalid record: {record}")
                        continue

                    # Extract required fields
                    subfolder = record.get("subfolder", "").strip()
                    merged_file = record.get("merged_file", {})
                    files = record.get("files", [])

                    if not subfolder or not merged_file or not files:
                        logging.warning(
                            f"Skipping incomplete record: missing subfolder, merged_file, or files"
                        )
                        continue

                    # Get merged file info
                    file_id = merged_file.get("file_id", "").strip()
                    email = merged_file.get("email", "").strip()

                    if not file_id or not email:
                        logging.warning(
                            f"Skipping record: missing file_id or email in merged_file"
                        )
                        continue

                    # Get or create GDR account
                    gdr_account_id = self.get_or_create_gdr_account(email)

                    # Process each file in the record
                    for file_info in files:
                        if not isinstance(file_info, dict):
                            continue

                        file_name = file_info.get("file_name", "").strip()
                        byte_range = file_info.get("byte_range", [])

                        if not file_name or len(byte_range) != 2:
                            logging.warning(f"Skipping invalid file info: {file_info}")
                            continue

                        # Validate that it's an audio file
                        if not self.is_valid_audio_file(file_name):
                            logging.warning(f"Skipping non-audio file: {file_name}")
                            continue

                        start_byte, end_byte = byte_range[0], byte_range[1]

                        # Check if GDR content already exists when doing incremental
                        if incremental:
                            self.cur.execute(
                                "SELECT 1 FROM content_gdr WHERE track_id IN "
                                "(SELECT track_id FROM tracks WHERE album_id IN "
                                "(SELECT album_id FROM albums WHERE path = ?) AND track_name = ?) "
                                "AND gdr_account_id = ? AND cid = ? AND start_byte = ? AND end_byte = ?",
                                (
                                    subfolder,
                                    file_name,
                                    gdr_account_id,
                                    file_id,
                                    start_byte,
                                    end_byte,
                                ),
                            )
                            if self.cur.fetchone():
                                skipped += 1
                                continue

                        # Get or create album and track
                        album_id = self.get_or_create_album(subfolder)
                        track_id = self.get_or_create_track(album_id, file_name)

                        # Add GDR content
                        if self.add_gdr_content(
                            track_id, gdr_account_id, file_id, start_byte, end_byte
                        ):
                            processed += 1
                        else:
                            skipped += 1

            self.cur.execute("COMMIT")
            logging.info(f"JSON files processed: {processed} added, {skipped} skipped")

        except Exception as e:
            self.cur.execute("ROLLBACK")
            logging.error(f"Error processing JSON files: {e}")
            raise

        return processed

    def parse_arw_file(self, arw_path: str, incremental: bool = False) -> int:
        """
        Parse ARW file and insert records.
        Returns number of records processed.
        """
        processed = 0
        skipped = 0

        self.cur.execute("BEGIN")

        try:
            with open(arw_path, "r") as arw_file:
                for line_num, line in enumerate(arw_file, 1):
                    line = line.strip()
                    if not line:
                        continue

                    parts = line.split(",", 1)
                    if len(parts) < 2:
                        logging.warning(f"Invalid line {line_num}: {line}")
                        continue

                    cid = parts[0].strip()
                    path = Path(parts[1].strip())

                    # Remove "data/" prefix if present
                    album_path = str(path.parent)
                    if album_path.startswith("data/"):
                        album_path = album_path[5:]

                    filename = str(path.name)

                    # Parse filename with optional numbering
                    match = re.search(
                        r"(.*)\.(opus|mp3|m4a|m4b)[.]?(\d*)$", filename, re.IGNORECASE
                    )
                    if not match:
                        logging.warning(f"Invalid audio file: {filename}")
                        continue

                    orig_filename = f"{match.group(1)}.{match.group(2)}"

                    # Check if CID already exists when doing incremental
                    if incremental:
                        self.cur.execute(
                            "SELECT 1 FROM content_cid WHERE cid = ?", (cid,)
                        )
                        if self.cur.fetchone():
                            skipped += 1
                            continue

                    album_id = self.get_or_create_album(album_path)
                    track_id = self.get_or_create_track(album_id, orig_filename)

                    if self.add_content(track_id, cid):
                        processed += 1
                    else:
                        skipped += 1

            self.cur.execute("COMMIT")
            logging.info(f"ARW file processed: {processed} added, {skipped} skipped")

        except Exception as e:
            self.cur.execute("ROLLBACK")
            logging.error(f"Error processing ARW file: {e}")
            raise

        return processed

    def vacuum(self):
        """Optimize database by running VACUUM."""
        logging.info("Running VACUUM to optimize database...")
        self.conn.execute("VACUUM")
        self.conn.commit()

    def get_stats(self) -> dict:
        """Get database statistics."""
        stats = {}

        self.cur.execute("SELECT COUNT(*) FROM albums")
        stats["albums"] = self.cur.fetchone()[0]

        self.cur.execute("SELECT COUNT(*) FROM tracks")
        stats["tracks"] = self.cur.fetchone()[0]

        self.cur.execute("SELECT COUNT(*) FROM content_cid")
        stats["content_cid"] = self.cur.fetchone()[0]

        self.cur.execute("SELECT COUNT(*) FROM gdr_accounts")
        stats["gdr_accounts"] = self.cur.fetchone()[0]

        self.cur.execute("SELECT COUNT(*) FROM content_gdr")
        stats["content_gdr"] = self.cur.fetchone()[0]

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

    parser.add_argument("nft_file", help="Path to NFT CSV file")
    parser.add_argument("arw_file", help="Path to ARW file")
    parser.add_argument("json_files", help="Comma-separated list of JSON files")
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
    if not Path(args.nft_file).exists():
        logging.error(f"NFT file not found: {args.nft_file}")
        sys.exit(1)

    if not Path(args.arw_file).exists():
        logging.error(f"ARW file not found: {args.arw_file}")
        sys.exit(1)

    # Parse JSON files argument
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
            # Create tables and indexes
            db_manager.create_tables()

            # Show initial stats
            if args.incremental and db_path.exists():
                initial_stats = db_manager.get_stats()
                logging.info(f"Initial database stats: {initial_stats}")

            # Process files
            nft_processed = db_manager.parse_nft_file(args.nft_file, args.incremental)
            arw_processed = db_manager.parse_arw_file(args.arw_file, args.incremental)

            # Process JSON files
            json_processed = 0
            if json_file_list:
                json_processed = db_manager.parse_json_files(
                    json_file_list, args.incremental
                )

            # Create indexes
            db_manager.create_indexes()

            # Optimize database
            if not args.no_vacuum:
                db_manager.vacuum()

            # Show final stats
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
    # Full rebuild
    python audio_db.py nft.csv arw.csv 001.json,002.json,003.json data.db --rebuild

    # Incremental update
    python audio_db.py nft.csv arw.csv 001.json,002.json data.db --incremental

    # Verbose logging
    python audio_db.py nft.csv arw.csv 001.json data.db --incremental --verbose

    # Skip vacuum optimization
    python audio_db.py nft.csv arw.csv 001.json data.db --no-vacuum

    # Without JSON files (empty list)
    python audio_db.py nft.csv arw.csv "" data.db --incremental
    """
    main()
