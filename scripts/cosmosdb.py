import sys
import sqlite3
import asyncio
import random
import json
import argparse
import logging
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any
from contextlib import asynccontextmanager

from azure.cosmos.aio import CosmosClient
from azure.cosmos.exceptions import CosmosHttpResponseError


class MigrationConfig:
    def __init__(
        self,
        db_path: str,
        cosmos_uri: str,
        cosmos_key: str,
        cosmos_db_name: str,
        cosmos_container: str,
        partition_key_path: str,
        batch_size: int = 400,
        max_retries: int = 10,
        base_delay: float = 0.5,
        restart_track_id: int = 1,
    ):
        self.db_path = db_path
        self.cosmos_uri = cosmos_uri
        self.cosmos_key = cosmos_key
        self.cosmos_db_name = cosmos_db_name
        self.cosmos_container = cosmos_container
        self.partition_key_path = partition_key_path
        self.batch_size = batch_size
        self.max_retries = max_retries
        self.base_delay = base_delay
        self.restart_track_id = restart_track_id

    def from_file(config_path: str):
        try:
            with open(config_path, "r", encoding="utf-8") as f:
                config = json.load(f)

            return MigrationConfig(
                db_path=config["db"],
                cosmos_uri=config["cosmos_uri"],
                cosmos_key=config["cosmos_key"],
                cosmos_db_name=config["cosmos_db_name"],
                cosmos_container=config["cosmos_container"],
                partition_key_path=config["partition_key_path"],
                batch_size=config.get("batch_size", 400),
                max_retries=config.get("max_retries", 10),
                base_delay=config.get("base_delay", 0.5),
                restart_track_id=config.get("restart_track_id", 1),
            )
        except (FileNotFoundError, KeyError, json.JSONDecodeError) as e:
            logging.error(f"Failed to load configuration: {e}")
            sys.exit(1)


class MigrationStats:
    def __init__(self):
        self.total_processed = 0
        self.total_batches = 0
        self.failed_documents = 0
        self.start_time = datetime.now()
        self.last_track_id = 0

    def update(self, batch_size: int, last_track_id: int):
        self.total_processed += batch_size
        self.total_batches += 1
        self.last_track_id = last_track_id

    def log_progress(self):
        elapsed = datetime.now() - self.start_time
        rate = (
            self.total_processed / elapsed.total_seconds()
            if elapsed.total_seconds() > 0
            else 0
        )
        logging.info(
            f"Progress: {self.total_processed} documents, {self.total_batches} batches, "
            f"{rate:.2f} docs/sec, last track_id: {self.last_track_id}"
        )


class CosmosDBMigrator:
    def __init__(self, config: MigrationConfig):
        self.config = config
        self.stats = MigrationStats()
        self.setup_logging()

    def setup_logging(self):
        log_format = "%(asctime)s - %(levelname)s - %(message)s"
        Path("logs").mkdir(exist_ok=True)

        # Configure Azure SDK logging to reduce verbosity
        azure_logger = logging.getLogger('azure')
        azure_logger.setLevel(logging.WARNING)
        
        # Configure requests logging to reduce verbosity
        requests_logger = logging.getLogger('urllib3')
        requests_logger.setLevel(logging.WARNING)

        logging.basicConfig(
            level=logging.INFO,
            format=log_format,
            handlers=[
                logging.FileHandler(
                    f"logs/migration_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"
                ),
                logging.StreamHandler(sys.stdout),
            ],
        )

    @asynccontextmanager
    async def get_cosmos_container(self):
        client = None
        try:
            client = CosmosClient(
                self.config.cosmos_uri, credential=self.config.cosmos_key
            )
            db = client.get_database_client(self.config.cosmos_db_name)
            container = db.get_container_client(self.config.cosmos_container)
            yield container
        except Exception as e:
            logging.error(f"Failed to connect to Cosmos DB: {e}")
            raise
        finally:
            if client:
                await client.close()

    def get_sqlite_connection(self) -> sqlite3.Connection:
        try:
            conn = sqlite3.connect(self.config.db_path)
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute("PRAGMA synchronous=NORMAL")
            conn.execute("PRAGMA cache_size=10000")
            conn.execute("PRAGMA temp_store=MEMORY")
            return conn
        except sqlite3.Error as e:
            logging.error(f"Failed to connect to SQLite database: {e}")
            sys.exit(1)

    async def upsert_with_retry(self, container, doc: Dict[str, Any]) -> bool:
        delay = self.config.base_delay

        for attempt in range(self.config.max_retries):
            try:
                await container.upsert_item(doc)
                return True

            except CosmosHttpResponseError as e:
                if e.status_code == 429:
                    retry_after = (
                        float(e.headers.get("x-ms-retry-after-ms", delay * 1000))
                        / 1000.0
                    )
                    logging.warning(
                        f"Rate limited (429). Retrying doc {doc.get('id')} after {retry_after:.2f}s "
                        f"(attempt {attempt + 1}/{self.config.max_retries})"
                    )
                    await asyncio.sleep(retry_after)
                    delay = min(delay * 2, 10) + random.uniform(0, 0.5)

                elif e.status_code == 413:
                    logging.error(f"Document {doc.get('id')} too large: {e}")
                    return False

                else:
                    logging.error(f"Cosmos DB error for doc {doc.get('id')}: {e}")
                    return False

            except Exception as e:
                logging.error(f"Unexpected error upserting doc {doc.get('id')}: {e}")
                return False

        logging.error(
            f"Failed to upsert doc {doc.get('id')} after {self.config.max_retries} retries"
        )
        return False

    async def send_bulk(self, container, docs: List[Dict[str, Any]]) -> int:
        if not docs:
            return 0

        logging.info(f"Processing batch of {len(docs)} documents...")

        semaphore = asyncio.Semaphore(20)

        async def limited_upsert(doc):
            async with semaphore:
                return await self.upsert_with_retry(container, doc)

        tasks = [limited_upsert(doc) for doc in docs]
        results = await asyncio.gather(*tasks, return_exceptions=True)

        successful = sum(1 for result in results if result is True)
        failed = len(docs) - successful

        if failed > 0:
            logging.warning(
                f"Batch completed: {successful} successful, {failed} failed"
            )
            self.stats.failed_documents += failed
        else:
            logging.info(f"Batch completed successfully: {successful} documents")

        return successful

    def build_document(
        self, track_id: int, track_name: str, album_id: int, path: str
    ) -> Dict[str, Any]:
        return {
            "id": str(track_id),
            "track_id": track_id,
            "track_name": track_name,
            "album": {"album_id": album_id, "path": path},
            "cids": [],
        }

    def add_content_to_document(
        self,
        doc: Dict[str, Any],
        content_cid: Optional[str],
        gdr_cid: Optional[str],
        start_byte: Optional[int],
        end_byte: Optional[int],
        gdr_account_id: Optional[int],
        email: Optional[str],
    ):
        if content_cid and content_cid not in doc["cids"]:
            doc["cids"].append(content_cid)

        if all([gdr_cid, start_byte is not None, end_byte is not None, gdr_account_id]):
            if gdr_cid not in doc["cids"]:
                doc["cids"].append(gdr_cid)

            doc["byte_range"] = [start_byte, end_byte]
            doc["gdr_account_id"] = gdr_account_id
            doc["email"] = email

    async def migrate_data(self):
        logging.info("Starting migration process...")
        logging.info(
            f"Configuration: batch_size={self.config.batch_size}, "
            f"restart_track_id={self.config.restart_track_id}"
        )

        conn = self.get_sqlite_connection()

        try:
            async with self.get_cosmos_container() as container:
                await self.process_data(conn, container)

        except Exception as e:
            logging.error(f"Migration failed: {e}")
            raise
        finally:
            conn.close()

        elapsed = datetime.now() - self.stats.start_time
        logging.info(f"Migration completed in {elapsed}")
        logging.info(f"Total processed: {self.stats.total_processed} documents")
        logging.info(f"Total batches: {self.stats.total_batches}")
        logging.info(f"Failed documents: {self.stats.failed_documents}")
        if self.stats.failed_documents > 0:
            logging.warning(
                "Migration completed with some failures. Check logs for details."
            )

    async def process_data(self, conn: sqlite3.Connection, container):
        query = """
        SELECT 
            t.track_id, 
            t.track_name, 
            a.album_id, 
            a.path,
            cc.cid as content_cid,
            cg.cid as gdr_cid,
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
        """

        cur = conn.cursor()
        cur.execute(query, (self.config.restart_track_id,))

        current_track_id = None
        current_doc = None
        batch = []

        try:
            for row in cur:
                (
                    track_id,
                    track_name,
                    album_id,
                    path,
                    content_cid,
                    gdr_cid,
                    start_byte,
                    end_byte,
                    gdr_account_id,
                    email,
                ) = row

                if current_track_id != track_id:
                    if current_doc:
                        batch.append(current_doc)

                        if len(batch) >= self.config.batch_size:
                            await self.process_batch(container, batch)
                            batch = []

                    current_doc = self.build_document(
                        track_id, track_name, album_id, path
                    )
                    current_track_id = track_id

                self.add_content_to_document(
                    current_doc,
                    content_cid,
                    gdr_cid,
                    start_byte,
                    end_byte,
                    gdr_account_id,
                    email,
                )

            if current_doc:
                batch.append(current_doc)

            if batch:
                await self.process_batch(container, batch)

        except Exception as e:
            logging.error(f"Error processing data: {e}")
            raise

    async def process_batch(self, container, batch: List[Dict[str, Any]]):
        if not batch:
            return

        track_ids = [doc["track_id"] for doc in batch]
        min_id, max_id = min(track_ids), max(track_ids)

        logging.info(
            f"Processing batch: track_ids {min_id}-{max_id} ({len(batch)} docs)"
        )

        successful = await self.send_bulk(container, batch)
        self.stats.update(successful, max_id)

        if self.stats.total_batches % 10 == 0:
            self.stats.log_progress()


def main():
    parser = argparse.ArgumentParser(
        description="Migrate data from SQLite to Cosmos DB with improved error handling and performance"
    )
    parser.add_argument(
        "--config", type=str, required=True, help="Path to the configuration JSON file"
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Perform a dry run without actually writing to Cosmos DB",
    )

    args = parser.parse_args()

    config = MigrationConfig.from_file(args.config)

    if args.dry_run:
        logging.info("DRY RUN MODE - No data will be written to Cosmos DB")
        return

    migrator = CosmosDBMigrator(config)

    try:
        asyncio.run(migrator.migrate_data())
    except KeyboardInterrupt:
        logging.info("Migration interrupted by user")
        sys.exit(1)
    except Exception as e:
        logging.error(f"Migration failed with error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
