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
from pathlib import Path
from typing import Set, Dict, List, Tuple
from audiodb import AudioDatabaseManager, setup_logging


class JSONProcessor(AudioDatabaseManager):
    """Processes JSON files for GDR content."""

    def parse_json_files(self, json_files: List[str], incremental: bool = False) -> int:
        """Parse JSON files and insert GDR records using optimized batch processing."""
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

        # Track what we need to update in cache
        albums_to_update = []
        tracks_to_update = []
        gdr_accounts_to_update = []

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
                        processed += self._flush_json_batches(
                            albums_batch,
                            tracks_batch,
                            gdr_accounts_batch,
                            gdr_content_batch,
                            albums_to_update,
                            tracks_to_update,
                            gdr_accounts_to_update,
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
                        albums_to_update,
                        tracks_to_update,
                        gdr_accounts_to_update,
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
                        processed += self._flush_json_batches(
                            albums_batch,
                            tracks_batch,
                            gdr_accounts_batch,
                            gdr_content_batch,
                            albums_to_update,
                            tracks_to_update,
                            gdr_accounts_to_update,
                            album_cache,
                            track_cache,
                            gdr_account_cache,
                        )

            # Final flush
            processed += self._flush_json_batches(
                albums_batch,
                tracks_batch,
                gdr_accounts_batch,
                gdr_content_batch,
                albums_to_update,
                tracks_to_update,
                gdr_accounts_to_update,
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

    def _flush_json_batches(
        self,
        albums_batch: List[str],
        tracks_batch: List[Tuple[int, str]],
        gdr_accounts_batch: List[str],
        gdr_content_batch: List[Tuple],
        albums_to_update: List[str],
        tracks_to_update: List[Tuple[int, str]],
        gdr_accounts_to_update: List[str],
        album_cache: Dict,
        track_cache: Dict,
        gdr_account_cache: Dict,
    ) -> int:
        """Flush JSON batches with optimized cache updates."""
        processed = 0

        # Insert albums and update cache
        if albums_batch:
            self.insert_albums_batch(albums_batch)
            self.bulk_update_album_cache(albums_to_update, album_cache)
            albums_batch.clear()
            albums_to_update.clear()

        # Insert GDR accounts and update cache
        if gdr_accounts_batch:
            self.insert_gdr_accounts_batch(gdr_accounts_batch)
            self.bulk_update_gdr_account_cache(
                gdr_accounts_to_update, gdr_account_cache
            )
            gdr_accounts_batch.clear()
            gdr_accounts_to_update.clear()

        # Insert tracks and update cache
        if tracks_batch:
            self.insert_tracks_batch(tracks_batch)
            self.bulk_update_track_cache(tracks_to_update, track_cache)
            tracks_batch.clear()
            tracks_to_update.clear()

        # Resolve IDs for GDR content and insert
        if gdr_content_batch:
            resolved_gdr_content = []
            for item in gdr_content_batch:
                if len(item) == 8:  # Extended format with references
                    (
                        _,
                        _,
                        file_id,
                        start_byte,
                        end_byte,
                        subfolder,
                        file_name,
                        email,
                    ) = item

                    # Get resolved IDs
                    album_id = album_cache.get(subfolder)
                    gdr_account_id = gdr_account_cache.get(email)

                    if album_id and gdr_account_id:
                        track_id = track_cache.get((album_id, file_name))
                        if track_id:
                            resolved_gdr_content.append(
                                (
                                    track_id,
                                    gdr_account_id,
                                    file_id,
                                    start_byte,
                                    end_byte,
                                )
                            )
                else:  # Standard format
                    resolved_gdr_content.append(item)

            processed += self.insert_content_gdr_batch(resolved_gdr_content)
            gdr_content_batch.clear()

        return processed

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
        albums_to_update,
        tracks_to_update,
        gdr_accounts_to_update,
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

        # Ensure GDR account exists
        if email not in gdr_account_cache:
            if email not in gdr_accounts_batch:
                gdr_accounts_batch.append(email)
                gdr_accounts_to_update.append(email)

        # Ensure album exists
        if subfolder not in album_cache:
            if subfolder not in albums_batch:
                albums_batch.append(subfolder)
                albums_to_update.append(subfolder)

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
            if album_id:
                track_key = (album_id, file_name)
                if track_key not in track_cache:
                    if track_key not in tracks_batch:
                        tracks_batch.append(track_key)
                        tracks_to_update.append(track_key)

            # Add to GDR content batch with extended format for ID resolution
            gdr_content_batch.append(
                (None, None, file_id, start_byte, end_byte, subfolder, file_name, email)
            )

        return "processed"

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
