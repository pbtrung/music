#!/usr/bin/env python3
"""
NFT CSV File Processor
Processes NFT CSV files where each row represents a unique track.
Enforces that track_count = content_cid_count (one CID per track).
"""

import argparse
import sys
import csv
import logging
import time
from pathlib import Path
from typing import Set, Dict, List, Tuple
from audiodb import AudioDatabaseManager, setup_logging


class NFTProcessor(AudioDatabaseManager):
    """Processes NFT CSV files where each row is a unique track with one CID."""

    def parse_nft_file(self, nft_path: str, incremental: bool = False) -> int:
        """Parse NFT CSV file and insert records using optimized batch processing."""
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

        # No duplicate tracking needed - each row creates a unique track

        # Initialize batches
        albums_batch = []
        tracks_batch = []
        content_batch = []

        # Track what we need to update in cache
        albums_to_update = []
        tracks_to_update = []

        self.cur.execute("BEGIN")
        try:
            with open(nft_path, "r") as nft_file:
                nft_reader = csv.reader(nft_file, delimiter=",")

                for row_num, row in enumerate(nft_reader, 1):
                    total_rows += 1

                    if row_num % 20000 == 0:
                        logging.info(f"Processing NFT row {row_num:,}...")
                        processed += self._flush_nft_batches(
                            albums_batch,
                            tracks_batch,
                            content_batch,
                            albums_to_update,
                            tracks_to_update,
                            album_cache,
                            track_cache,
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
                        if (
                            non_audio_files <= 10
                        ):  # Log first 10 non-audio files for debugging
                            logging.debug(f"Non-audio file skipped: {track_name}")
                        continue

                    # Skip if CID already exists (for incremental updates)
                    if incremental and cid in existing_cids:
                        skipped += 1
                        continue

                    # Each row creates a unique track - no duplicate checking needed

                    # Prepare album
                    if album_path not in album_cache and album_path not in albums_batch:
                        albums_batch.append(album_path)
                        albums_to_update.append(album_path)

                    # Each row creates a unique track entry
                    # We use a unique key that includes the CID to ensure uniqueness
                    unique_track_key = (album_path, track_name, cid)
                    tracks_batch.append(unique_track_key)
                    tracks_to_update.append(unique_track_key)

                    # Store content for processing after cache updates
                    content_batch.append(unique_track_key)

                    if len(content_batch) >= self.BATCH_SIZE:
                        processed += self._flush_nft_batches(
                            albums_batch,
                            tracks_batch,
                            content_batch,
                            albums_to_update,
                            tracks_to_update,
                            album_cache,
                            track_cache,
                        )

            # Final flush
            processed += self._flush_nft_batches(
                albums_batch,
                tracks_batch,
                content_batch,
                albums_to_update,
                tracks_to_update,
                album_cache,
                track_cache,
            )
            self.cur.execute("COMMIT")

        except Exception as e:
            self.cur.execute("ROLLBACK")
            logging.error(f"Error processing NFT file: {e}")
            raise

        # Verify track_count = content_cid_count
        self._verify_track_content_consistency()

        # Calculate and log the discrepancy
        expected_from_file = total_rows
        actual_processed = processed
        missing_rows = expected_from_file - actual_processed

        if missing_rows != 0:
            logging.warning(f"Row count discrepancy detected:")
            logging.warning(f"  - Expected from file: {expected_from_file:,}")
            logging.warning(f"  - Actually processed: {actual_processed:,}")
            logging.warning(f"  - Missing rows: {missing_rows:,}")
            logging.warning(
                f"  - Breakdown: {invalid_rows:,} invalid + {non_audio_files:,} non-audio + {skipped:,} skipped = {invalid_rows + non_audio_files + skipped:,}"
            )

        self._log_nft_stats(
            start_time,
            total_rows,
            processed,
            skipped,
            invalid_rows,
            non_audio_files,
        )
        return processed

    def _flush_nft_batches(
        self,
        albums_batch: List[str],
        tracks_batch: List[Tuple[str, str, str]],  # (album_path, track_name, cid)
        content_batch: List[Tuple[str, str, str]],  # (album_path, track_name, cid)
        albums_to_update: List[str],
        tracks_to_update: List[Tuple[str, str, str]],
        album_cache: Dict,
        track_cache: Dict,
    ) -> int:
        """Flush NFT batches with optimized cache updates."""
        processed = 0

        # Insert albums and update cache
        if albums_batch:
            self.insert_albums_batch(albums_batch)
            self.bulk_update_album_cache(albums_to_update, album_cache)
            albums_batch.clear()
            albums_to_update.clear()

        # Insert tracks - each track is unique per row
        if tracks_batch:
            track_records = []
            for album_path, track_name, cid in tracks_batch:
                album_id = album_cache.get(album_path)
                if album_id is not None:
                    track_records.append((album_id, track_name))
                else:
                    logging.warning(
                        f"Album not found for track: {album_path}/{track_name}"
                    )

            if track_records:
                self.insert_tracks_batch_nft(track_records)

                # Update track cache with the newly inserted tracks
                # We need to get the actual track_ids from the database
                self._update_track_cache_after_insert(
                    tracks_to_update, album_cache, track_cache
                )

            tracks_batch.clear()
            tracks_to_update.clear()

        # Process content batch
        if content_batch:
            resolved_content = []
            for album_path, track_name, cid in content_batch:
                album_id = album_cache.get(album_path)
                if album_id is not None:
                    # For NFT, we need to find the specific track that was just inserted
                    # Since we can have multiple tracks with the same name but different CIDs,
                    # we'll get the most recently inserted track for this album/track_name combination
                    track_id = self._get_latest_track_id(album_id, track_name)
                    if track_id is not None:
                        resolved_content.append((track_id, cid))
                    else:
                        logging.warning(f"Track not found: {album_path}/{track_name}")
                else:
                    logging.warning(f"Album not found: {album_path}")

            processed += self.insert_content_cid_batch(resolved_content)
            content_batch.clear()

        return processed

    def _get_latest_track_id(self, album_id: int, track_name: str) -> int:
        """Get the most recently inserted track_id for album_id/track_name combination."""
        self.cur.execute(
            """
            SELECT track_id FROM tracks 
            WHERE album_id = ? AND track_name = ? 
            ORDER BY track_id DESC 
            LIMIT 1
            """,
            (album_id, track_name),
        )
        result = self.cur.fetchone()
        return result[0] if result else None

    def _update_track_cache_after_insert(
        self,
        tracks_to_update: List[Tuple[str, str, str]],
        album_cache: Dict,
        track_cache: Dict,
    ):
        """Update track cache after inserting new tracks."""
        # Since we can have duplicate track names, we'll update the cache
        # with the latest track_id for each album/track_name combination
        for album_path, track_name, cid in tracks_to_update:
            album_id = album_cache.get(album_path)
            if album_id is not None:
                track_id = self._get_latest_track_id(album_id, track_name)
                if track_id is not None:
                    # Update cache with the latest track_id
                    track_cache[(album_id, track_name)] = track_id

    def insert_tracks_batch_nft(self, tracks: List[Tuple[int, str]]):
        """Insert tracks in batch for NFT processing (allows duplicates)."""
        if not tracks:
            return
        # Use INSERT instead of INSERT OR IGNORE to allow duplicate track names
        self.cur.executemany(
            "INSERT INTO tracks (album_id, track_name) VALUES (?, ?)",
            tracks,
        )

    def _verify_track_content_consistency(self):
        """Verify that track_count equals content_cid_count."""
        self.cur.execute("SELECT COUNT(*) FROM tracks")
        track_count = self.cur.fetchone()[0]

        self.cur.execute("SELECT COUNT(*) FROM content_cid")
        content_count = self.cur.fetchone()[0]

        if track_count != content_count:
            logging.error(
                f"Consistency check failed: {track_count} tracks != {content_count} content_cids"
            )
            raise ValueError(
                f"Track count ({track_count}) does not match content CID count ({content_count})"
            )
        else:
            logging.info(
                f"Consistency check passed: {track_count} tracks = {content_count} content_cids"
            )

    def _log_nft_stats(
        self,
        start_time: float,
        total_rows: int,
        processed: int,
        skipped: int,
        invalid_rows: int,
        non_audio_files: int,
    ):
        """Log NFT processing statistics."""
        elapsed = time.time() - start_time
        logging.info(f"NFT file processing complete:")
        logging.info(f"  - Total rows processed: {total_rows:,}")
        logging.info(f"  - Records added: {processed:,}")
        logging.info(f"  - Records skipped: {skipped:,}")
        logging.info(f"  - Invalid rows: {invalid_rows:,}")
        logging.info(f"  - Non-audio files: {non_audio_files:,}")
        logging.info(f"  - Processing time: {elapsed:.2f} seconds")
        if total_rows > 0:
            logging.info(f"  - Processing rate: {total_rows / elapsed:.0f} rows/second")


def main():
    """Main entry point for NFT processing."""

    parser = argparse.ArgumentParser(description="NFT CSV File Processor")
    parser.add_argument("nft_file", help="Path to NFT CSV file")
    parser.add_argument("database", help="Path to SQLite database file")
    parser.add_argument(
        "-i",
        "--incremental",
        action="store_true",
        help="Perform incremental update (skip existing CIDs)",
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

    # Validate input file
    if not Path(args.nft_file).exists():
        logging.error(f"NFT file not found: {args.nft_file}")
        sys.exit(1)

    # Handle database creation/rebuild
    db_path = Path(args.database)
    if args.rebuild and db_path.exists():
        logging.info(f"Rebuilding database: {db_path}")
        db_path.unlink()

    # Process file
    try:
        with NFTProcessor(str(db_path)) as processor:
            processor.create_tables()

            if args.incremental and db_path.exists():
                initial_stats = processor.get_stats()
                logging.info(f"Initial database stats: {initial_stats}")

            processed = processor.parse_nft_file(args.nft_file, args.incremental)

            processor.create_indexes()

            if not args.no_vacuum:
                processor.vacuum()

            final_stats = processor.get_stats()
            logging.info(f"Final database stats: {final_stats}")
            logging.info(f"Total records processed: {processed}")

    except Exception as e:
        logging.error(f"NFT processing failed: {e}")
        sys.exit(1)

    logging.info("NFT processing completed successfully")


if __name__ == "__main__":
    main()
