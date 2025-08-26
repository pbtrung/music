import sys
import sqlite3
import asyncio
import random
import json
import argparse
from azure.cosmos.aio import CosmosClient
from azure.cosmos.exceptions import CosmosHttpResponseError

# CLI args
parser = argparse.ArgumentParser(description="Migrate data from SQLite to Cosmos DB")
parser.add_argument(
    "--config", type=str, required=True, help="Path to the configuration file"
)
args = parser.parse_args()

# Load configuration
with open(args.config, "r", encoding="utf-8") as f:
    config = json.load(f)

SQLITE_DB = config["db"]
COSMOS_URI = config["cosmos_uri"]
COSMOS_KEY = config["cosmos_key"]
COSMOS_DB_NAME = config["cosmos_db_name"]
COSMOS_CONTAINER = config["cosmos_container"]
PARTITION_KEY_PATH = config["partition_key_path"]
BATCH_SIZE = 400

MAX_RETRIES = 10
BASE_DELAY = 0.5
RESTART_TRACK_ID = config.get("restart_track_id", 1)


async def upsert_with_retry(container, doc):
    """Upsert a document with exponential backoff on 429 errors."""
    delay = BASE_DELAY
    for attempt in range(MAX_RETRIES):
        try:
            await container.upsert_item(doc)
            return
        except CosmosHttpResponseError as e:
            if e.status_code == 429:
                retry_after = (
                    float(e.headers.get("x-ms-retry-after-ms", delay * 1000)) / 1000.0
                )
                print(
                    f"429 TooManyRequests. Retrying after {retry_after:.2f}s (attempt {attempt+1})..."
                )
                await asyncio.sleep(retry_after)
                # exponential + jitter
                delay = min(delay * 2, 10) + random.uniform(0, 0.5)
            else:
                raise
    sys.exit(
        f"Migration stopped: Failed to upsert doc {doc.get('id')} after {MAX_RETRIES} retries."
    )


async def send_bulk(container, docs):
    """Upsert all docs in the batch concurrently with retries."""
    tasks = [upsert_with_retry(container, doc) for doc in docs]
    await asyncio.gather(*tasks)
    print(f"Inserted {len(docs)} docs.")


async def migrate_sequential():
    client = CosmosClient(COSMOS_URI, credential=COSMOS_KEY)
    db = client.get_database_client(COSMOS_DB_NAME)
    container = db.get_container_client(COSMOS_CONTAINER)

    conn = sqlite3.connect(SQLITE_DB)

    cur = conn.cursor()
    cur.execute(
        """
        SELECT t.track_id, t.track_name, a.album_id, a.path, c.cid
        FROM tracks t
        JOIN albums a ON t.album_id = a.album_id
        LEFT JOIN content c ON t.track_id = c.track_id
        WHERE t.track_id > ?
        ORDER BY t.track_id
        """,
        (RESTART_TRACK_ID,),
    )

    current_track_id = None
    current_doc = None
    batch = []

    try:
        for track_id, track_name, album_id, path, cid in cur:
            if current_track_id != track_id:
                if current_doc:
                    batch.append(current_doc)
                    if len(batch) >= BATCH_SIZE:
                        ids = [doc["track_id"] for doc in batch]
                        print(f"Batch: min_id = {min(ids)}, max_id = {max(ids)}")
                        await send_bulk(container, batch)
                        batch = []
                current_doc = {
                    "id": str(track_id),
                    "track_id": track_id,
                    "track_name": track_name,
                    "album": {"album_id": album_id, "path": path},
                    "cids": [],
                }
                current_track_id = track_id

            if cid:
                current_doc["cids"].append(cid)

        if current_doc:
            batch.append(current_doc)
        if batch:
            ids = [doc["track_id"] for doc in batch]
            print(f"Final batch: min_id = {min(ids)}, max_id = {max(ids)}")
            await send_bulk(container, batch)

    except Exception as e:
        print(f"Migration failed: {e}")
        sys.exit(-1)
    finally:
        conn.close()
        await client.close()

    print("Migration complete.")


if __name__ == "__main__":
    asyncio.run(migrate_sequential())
