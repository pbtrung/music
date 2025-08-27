#!/usr/bin/env python3
"""
JSON File Processor
Processes JSON files containing GDR (Google Drive) records and inserts them into the audio database.
"""

import argparse
import sys
import json
import logging
import time
import sqlite3
from pathlib import Path
from typing import Dict, List, Tuple
from audiodb import AudioDatabaseManager, setup_logging


class JSONProcessor(AudioDatabaseManager):
    """Processes JSON files for GDR content (sequential insert mode)."""

    def parse_json_files(self, json_files: List[str], incremental: bool = False) -> int:
        """Parse JSON files and insert GDR records (simple per-record processing)."""
        logging.info(f"Parsing {len(json_files)} JSON files (sequential mode)")
        start_time = time.time()

        processed = 0
        album_cache: Dict[str, int] = {}
        track_cache: Dict[Tuple[int, str], int] = {}
        gdr_account_cache: Dict[str, int] = {}

        self.cur.execute("BEGIN")

        try:
            for file_num, json_file_path in enumerate(json_files, 1):
                logging.info(
                    f"Processing JSON file {file_num}/{len(json_files)}: {json_file_path}"
                )
                with open(json_file_path, "r", encoding="utf-8") as f:
                    try:
                        data = json.load(f)
                    except json.JSONDecodeError as e:
                        logging.error(f"Invalid JSON in {json_file_path}: {e}")
                        continue

                for record in data:
                    subfolder = record.get("subfolder", "").strip()
                    merged_file = record.get("merged_file", {})
                    files = record.get("files", [])

                    file_id = merged_file.get("file_id", "").strip()
                    email = merged_file.get("email", "").strip()
                    if not subfolder or not file_id or not email or not files:
                        logging.warning(f"Invalid record skipped in {json_file_path}")
                        continue

                    # ensure album
                    album_id = album_cache.get(subfolder)
                    if not album_id:
                        self.cur.execute(
                            "SELECT album_id FROM albums WHERE path = ?", (subfolder,)
                        )
                        row = self.cur.fetchone()
                        if not row:
                            self.cur.execute(
                                "INSERT INTO albums (path) VALUES (?)", (subfolder,)
                            )
                            album_id = self.cur.lastrowid
                        else:
                            album_id = row[0]
                        album_cache[subfolder] = album_id

                    # ensure gdr_account
                    gdr_account_id = gdr_account_cache.get(email)
                    if not gdr_account_id:
                        self.cur.execute(
                            "SELECT gdr_account_id FROM gdr_accounts WHERE email = ?",
                            (email,),
                        )
                        row = self.cur.fetchone()
                        if not row:
                            self.cur.execute(
                                "INSERT INTO gdr_accounts (email) VALUES (?)", (email,)
                            )
                            gdr_account_id = self.cur.lastrowid
                        else:
                            gdr_account_id = row[0]
                        gdr_account_cache[email] = gdr_account_id

                    for file_info in files:
                        file_name = file_info.get("file_name", "").strip()
                        byte_range = file_info.get("byte_range", [])
                        if not file_name or len(byte_range) != 2:
                            continue
                        if not self.is_valid_audio_file(file_name):
                            continue

                        start_byte, end_byte = byte_range

                        # ensure track
                        track_id = track_cache.get((album_id, file_name))
                        if not track_id:
                            self.cur.execute(
                                "SELECT track_id FROM tracks WHERE album_id = ? AND track_name = ?",
                                (album_id, file_name),
                            )
                            row = self.cur.fetchone()
                            if not row:
                                self.cur.execute(
                                    "INSERT INTO tracks (album_id, track_name) VALUES (?, ?)",
                                    (album_id, file_name),
                                )
                                track_id = self.cur.lastrowid
                            else:
                                track_id = row[0]
                            track_cache[(album_id, file_name)] = track_id

                        # insert content_gdr
                        try:
                            self.cur.execute(
                                "INSERT INTO content_gdr (track_id, gdr_account_id, cid, start_byte, end_byte) "
                                "VALUES (?, ?, ?, ?, ?)",
                                (
                                    track_id,
                                    gdr_account_id,
                                    file_id,
                                    start_byte,
                                    end_byte,
                                ),
                            )
                            processed += 1
                        except sqlite3.Error as error:
                            logging.error(f"Error inserting content_gdr: {error}")
                            self.cur.execute("ROLLBACK")
                            raise

            self.cur.execute("COMMIT")
        except Exception as e:
            self.cur.execute("ROLLBACK")
            logging.error(f"Error during JSON parsing: {e}")
            raise

        elapsed = time.time() - start_time
        logging.info(
            f"Processed {processed} GDR content records in {elapsed:.2f} seconds"
        )
        return processed


def main():
    """Main entry point for JSON processing."""

    parser = argparse.ArgumentParser(description="JSON File Processor for GDR content")
    parser.add_argument("json_files", nargs="+", help="Paths to JSON files to process")
    parser.add_argument("database", help="Path to SQLite database file")
    parser.add_argument(
        "-i",
        "--incremental",
        action="store_true",
        help="Perform incremental update (skip existing content)",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Enable verbose logging"
    )
    parser.add_argument(
        "--rebuild",
        action="store_true",
        help="Force rebuild by deleting existing database",
    )
    parser.add_argument(
        "--no-vacuum", action="store_true", help="Skip database VACUUM optimization"
    )

    args = parser.parse_args()
    setup_logging(args.verbose)

    # Validate input files
    for json_file in args.json_files:
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
        with JSONProcessor(str(db_path)) as processor:
            processor.create_tables()

            if args.incremental and db_path.exists():
                initial_stats = processor.get_stats()
                logging.info(f"Initial database stats: {initial_stats}")

            processed = processor.parse_json_files(args.json_files, args.incremental)

            processor.create_indexes()

            if not args.no_vacuum:
                processor.vacuum()

            final_stats = processor.get_stats()
            logging.info(f"Final database stats: {final_stats}")
            logging.info(f"Total GDR records processed: {processed}")

    except Exception as e:
        logging.error(f"JSON processing failed: {e}")
        sys.exit(1)

    logging.info("JSON processing completed successfully")


if __name__ == "__main__":
    main()
