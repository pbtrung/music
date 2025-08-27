#!/usr/bin/env python3
"""
ARW File Processor
Processes ARW files and inserts records into the audio database.
Optimized for sorted structure with track grouping.
"""

import argparse
import sys
import re
import logging
import time
from pathlib import Path
from typing import Set, Dict, List, Tuple
from audiodb import AudioDatabaseManager, setup_logging


class ARWProcessor(AudioDatabaseManager):
    """Processes ARW files with track grouping optimization."""

    def parse_arw_file(self, arw_path: str, incremental: bool = False) -> int:
        """Parse ARW file and insert records using optimized batch processing with track grouping."""
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

        # Track what we need to update in cache
        albums_to_update = []
        tracks_to_update = []

        # Track grouping optimization
        current_track = None
        current_album_id = None
        current_track_id = None
        # Store chunks for current track
        track_chunks = []

        self.cur.execute("BEGIN")
        try:
            with open(arw_path, "r") as arw_file:
                for line_num, line in enumerate(arw_file, 1):
                    total_lines += 1

                    if line_num % 20000 == 0:
                        logging.info(f"Processing ARW line {line_num:,}...")
                        # Flush current track chunks before batch flush
                        if track_chunks and current_track_id is not None:
                            chunk_results = self._process_track_chunks(
                                track_chunks,
                                current_track_id,
                                content_batch,
                                existing_cids,
                                incremental,
                            )
                            processed += chunk_results["processed"]
                            skipped += chunk_results["skipped"]
                            track_chunks.clear()

                        processed += self._flush_arw_batches(
                            albums_batch,
                            tracks_batch,
                            content_batch,
                            albums_to_update,
                            tracks_to_update,
                            album_cache,
                            track_cache,
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

                    base_name = match.group(1)
                    extension = match.group(2)
                    chunk_num = match.group(3)
                    orig_filename = f"{base_name}.{extension}"

                    # Create track identifier (album_path + orig_filename)
                    track_identifier = (album_path, orig_filename)

                    # Check if we're starting a new track
                    if current_track != track_identifier:
                        # Process previous track's chunks if any
                        if track_chunks and current_track_id is not None:
                            chunk_results = self._process_track_chunks(
                                track_chunks,
                                current_track_id,
                                content_batch,
                                existing_cids,
                                incremental,
                            )
                            processed += chunk_results["processed"]
                            skipped += chunk_results["skipped"]

                        # Start new track
                        current_track = track_identifier
                        track_chunks = []

                        # Ensure album exists
                        if album_path not in album_cache:
                            if album_path not in albums_batch:
                                albums_batch.append(album_path)
                                albums_to_update.append(album_path)

                        album_id = album_cache.get(album_path)
                        if album_id is not None:
                            current_album_id = album_id
                            # Ensure track exists
                            track_key = (album_id, orig_filename)
                            if track_key not in track_cache:
                                # Insert new track immediately so we can get track_id
                                self.insert_tracks_batch([(album_id, orig_filename)])
                                self.bulk_update_track_cache([track_key], track_cache)

                            current_track_id = track_cache.get(track_key)
                            if current_track_id is None:
                                # Fallback: fetch latest inserted track id
                                current_track_id = self._get_latest_track_id(
                                    album_id, orig_filename
                                )
                                if current_track_id:
                                    track_cache[track_key] = current_track_id
                        else:
                            current_album_id = None
                            current_track_id = None

                    # Add chunk to current track
                    track_chunks.append(
                        {
                            "cid": cid,
                            "chunk_num": chunk_num,
                            "album_path": album_path,
                            "orig_filename": orig_filename,
                        }
                    )

                    # If content batch gets too large, flush it
                    if len(content_batch) >= self.BATCH_SIZE:
                        processed += self._flush_arw_batches(
                            albums_batch,
                            tracks_batch,
                            content_batch,
                            albums_to_update,
                            tracks_to_update,
                            album_cache,
                            track_cache,
                        )

                # Process final track's chunks
                if track_chunks and current_track_id is not None:
                    chunk_results = self._process_track_chunks(
                        track_chunks,
                        current_track_id,
                        content_batch,
                        existing_cids,
                        incremental,
                    )
                    processed += chunk_results["processed"]
                    skipped += chunk_results["skipped"]

                # Final flush
                processed += self._flush_arw_batches(
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

    def _flush_arw_batches(
        self,
        albums_batch: List[str],
        tracks_batch: List[Tuple[int, str]],
        content_batch: List[Tuple[int, str]],
        albums_to_update: List[str],
        tracks_to_update: List[Tuple[int, str]],
        album_cache: Dict,
        track_cache: Dict,
    ) -> int:
        """Flush ARW batches with optimized cache updates."""
        processed = 0

        # Insert albums and update cache
        if albums_batch:
            self.insert_albums_batch(albums_batch)
            self.bulk_update_album_cache(albums_to_update, album_cache)
            albums_batch.clear()
            albums_to_update.clear()

        # Insert tracks and update cache
        if tracks_batch:
            self.insert_tracks_batch(tracks_batch)
            self.bulk_update_track_cache(tracks_to_update, track_cache)
            tracks_batch.clear()
            tracks_to_update.clear()

        # Insert content
        if content_batch:
            processed += self.insert_content_cid_batch(content_batch)
            content_batch.clear()

        return processed

    def _process_track_chunks(
        self, track_chunks, track_id, content_batch, existing_cids, incremental
    ):
        """Process all chunks for a single track, taking advantage of sorted structure."""
        if not track_chunks or track_id is None:
            return {"processed": 0, "skipped": 0}

        processed_count = 0
        skipped_count = 0

        for chunk in track_chunks:
            cid = chunk["cid"]

            if incremental and cid in existing_cids:
                skipped_count += 1
                continue

            content_batch.append((track_id, cid))
            processed_count += 1

        if len(track_chunks) > 10:
            logging.debug(
                f"Processed track '{track_chunks[0]['orig_filename']}' with {len(track_chunks)} chunks"
            )

        return {"processed": processed_count, "skipped": skipped_count}

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


def main():
    """Main entry point for ARW processing."""

    parser = argparse.ArgumentParser(description="ARW File Processor")
    parser.add_argument("arw_file", help="Path to ARW file")
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

            if args.incremental and db_path.exists():
                initial_stats = processor.get_stats()
                logging.info(f"Initial database stats: {initial_stats}")

            processed = processor.parse_arw_file(args.arw_file, args.incremental)

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
