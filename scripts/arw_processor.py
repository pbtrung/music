#!/usr/bin/env python3
"""
ARW File Processor
Processes ARW files and inserts records into the audio database.
Optimized for reduced queries by caching and batching with progress logging.
"""

import argparse
import sys
import re
import logging
import time
from pathlib import Path
import sqlite3
from typing import Dict, Tuple
from audiodb import AudioDatabaseManager, setup_logging


class ARWProcessor(AudioDatabaseManager):
    """Processes ARW files with optimized insert logic."""

    def parse_arw_file(self, arw_path: str, incremental: bool = False) -> int:
        logging.info(f"Parsing ARW file: {arw_path}")
        start_time = time.time()

        processed = 0
        invalid_lines = 0
        invalid_audio_files = 0
        total_lines = 0

        album_cache: Dict[str, int] = {}
        track_cache: Dict[Tuple[int, str], int] = {}

        self.cur.execute("BEGIN")
        try:
            with open(arw_path, "r") as arw_list:
                for line_num, line in enumerate(arw_list, 1):
                    total_lines += 1
                    line = line.strip()
                    if not line:
                        continue

                    parts = line.split(",", 1)
                    if len(parts) < 2:
                        invalid_lines += 1
                        continue

                    cid, path_str = parts[0].strip(), parts[1].strip()
                    path = Path(path_str)

                    dn = str(path.parent)
                    if dn.startswith("data/"):
                        dn = dn[5:]
                    fn = str(path.name).strip()

                    match = re.search(
                        r"(.*)\.(opus|mp3|m4a|m4b)[.]?(\d*)$", fn, re.IGNORECASE
                    )
                    if not match:
                        invalid_audio_files += 1
                        logging.warning(f"Invalid audio file: {path}")
                        continue

                    orig_fn = f"{match.group(1)}.{match.group(2)}"

                    # Album caching
                    album_id = album_cache.get(dn)
                    if album_id is None:
                        self.cur.execute(
                            "SELECT album_id FROM albums WHERE path = ?", (dn,)
                        )
                        row = self.cur.fetchone()
                        if not row:
                            self.cur.execute(
                                "INSERT INTO albums (path) VALUES (?)", (dn,)
                            )
                            album_id = self.cur.lastrowid
                            logging.debug(f"Inserted new album: {dn} (id={album_id})")
                        else:
                            album_id = row[0]
                            logging.debug(f"Found existing album: {dn} (id={album_id})")
                        album_cache[dn] = album_id

                    # Track caching
                    track_key = (album_id, orig_fn)
                    track_id = track_cache.get(track_key)
                    if track_id is None:
                        self.cur.execute(
                            "SELECT track_id FROM tracks WHERE album_id = ? AND track_name = ?",
                            (album_id, orig_fn),
                        )
                        row = self.cur.fetchone()
                        if not row:
                            self.cur.execute(
                                "INSERT INTO tracks (album_id, track_name) VALUES (?, ?)",
                                (album_id, orig_fn),
                            )
                            track_id = self.cur.lastrowid
                            logging.debug(
                                f"Inserted new track: {orig_fn} (id={track_id})"
                            )
                        else:
                            track_id = row[0]
                            logging.debug(
                                f"Found existing track: {orig_fn} (id={track_id})"
                            )
                        track_cache[track_key] = track_id

                    try:
                        self.cur.execute(
                            "INSERT INTO content_cid (track_id, cid) VALUES (?, ?)",
                            (track_id, cid),
                        )
                        processed += 1
                        if processed % 10000 == 0:
                            logging.info(
                                f"Inserted {processed:,} content records so far (line {line_num:,})"
                            )
                    except sqlite3.IntegrityError:
                        # Skip duplicates if constraint exists
                        continue
                    except sqlite3.Error as error:
                        logging.error(
                            f"Error inserting content: {error}, track_id={track_id}, cid={cid}"
                        )
                        self.cur.execute("ROLLBACK")
                        sys.exit(-1)

            self.cur.execute("COMMIT")

        except Exception as e:
            self.cur.execute("ROLLBACK")
            logging.error(f"Error processing ARW file: {e}")
            raise

        elapsed = time.time() - start_time
        logging.info(f"ARW file processing complete:")
        logging.info(f"  - Total lines processed: {total_lines:,}")
        logging.info(f"  - Records added: {processed:,}")
        logging.info(f"  - Invalid lines: {invalid_lines:,}")
        logging.info(f"  - Invalid audio files: {invalid_audio_files:,}")
        logging.info(f"  - Processing time: {elapsed:.2f} seconds")
        logging.info(f"  - Processing rate: {total_lines / elapsed:.0f} lines/second")

        return processed


def main():
    parser = argparse.ArgumentParser(description="ARW File Processor")
    parser.add_argument("arw_file", help="Path to ARW file")
    parser.add_argument("database", help="Path to SQLite database file")
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

    if not Path(args.arw_file).exists():
        logging.error(f"ARW file not found: {args.arw_file}")
        sys.exit(1)

    db_path = Path(args.database)
    if args.rebuild and db_path.exists():
        logging.info(f"Rebuilding database: {db_path}")
        db_path.unlink()

    try:
        with ARWProcessor(str(db_path)) as processor:
            processor.create_tables()
            processed = processor.parse_arw_file(args.arw_file)
            processor.create_indexes()

            if not args.no_vacuum:
                processor.vacuum()

            final_stats = processor.get_stats()
            logging.info(f"Final database stats: {final_stats}")
            logging.info(f"Total records processed: {processed}")

    except Exception as e:
        logging.error(f"ARW processing failed: {e}")
        sys.exit(1)

    logging.info("ARW processing completed successfully")


if __name__ == "__main__":
    main()
