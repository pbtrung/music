import os
import json
import re
import sys
import hashlib
import time


def get_next_index(dest_folder, ext):
    """Find the next available 6-digit index in the central merged folder."""
    existing = [
        int(m.group(1))
        for f in os.listdir(dest_folder)
        if (m := re.match(r"^(\d{6})\." + re.escape(ext) + r"$", f))
    ]
    return max(existing, default=0) + 1


def file_sha256(path):
    """Compute SHA-256 hash of a file efficiently. Returns (hex_digest, seconds)."""
    h = hashlib.sha256()
    start = time.perf_counter()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    elapsed = time.perf_counter() - start
    return h.hexdigest(), elapsed


def merge_files_in_subfolders(root_folder):
    merged_folder = os.path.join(root_folder, "merged_bins")
    os.makedirs(merged_folder, exist_ok=True)

    top_index = []  # collect summary across all subfolders

    for subdir, _, files in os.walk(root_folder):
        if subdir == root_folder or not files:
            continue  # skip root itself and empty folders

        print(f"\nProcessing folder: {os.path.relpath(subdir, root_folder)}")
        files = sorted(files)
        index_data = []
        offset = 0
        merged_files = []
        merged_path = None
        merged_file = None
        merge_start = None

        try:
            for fname in files:
                if re.match(r"^\d{6}\.(bin|json)$", fname):
                    print(f"  Skipping generated file: {fname}")
                    continue

                fpath = os.path.join(subdir, fname)
                size = os.path.getsize(fpath)
                if size == 0:
                    print(f"  Skipping empty file: {fname}")
                    continue

                if merged_path is None:
                    idx = get_next_index(merged_folder, "bin")
                    bin_name = f"{idx:06d}.bin"
                    merged_path = os.path.join(merged_folder, bin_name)
                    merged_file = open(merged_path, "wb")
                    merge_start = time.perf_counter()
                    print(f"  Creating merged file: {bin_name}")

                with open(fpath, "rb") as infile:
                    data = infile.read()
                    merged_file.write(data)

                entry = {
                    "file_name": fname,
                    "file_size": size,
                    "byte_range": [offset, offset + size - 1],
                }
                index_data.append(entry)
                merged_files.append(fpath)
                offset += size
                print(f"  Added {fname} ({size} bytes)")

            if merged_file:
                merged_file.close()
                merged_size = os.path.getsize(merged_path)
                if merged_size > 0:
                    hash_val, hash_time = file_sha256(merged_path)
                    merge_time = time.perf_counter() - merge_start
                    print(
                        f"  Finalized {os.path.basename(merged_path)} "
                        f"({merged_size} bytes, sha256={hash_val[:12]}...)"
                    )
                    print(
                        f"    Merge time: {merge_time:.3f}s, Hash time: {hash_time:.3f}s"
                    )
                    top_index.append(
                        {
                            "subfolder": os.path.relpath(subdir, root_folder),
                            "merged_file": {
                                "name": os.path.basename(merged_path),
                                "size": merged_size,
                                "sha256": hash_val,
                            },
                            "files": index_data,
                        }
                    )
                    # for f in merged_files:
                    #     try:
                    #         os.remove(f)
                    #         print(f"  Deleted original {f}")
                    #     except Exception as e:
                    #         print(f"  Warning: could not delete {f}: {e}")
                else:
                    os.remove(merged_path)
                    print("  Removed empty merged file (0 bytes)")

        finally:
            if merged_file and not merged_file.closed:
                merged_file.close()

    if top_index:
        top_index_path = os.path.join(merged_folder, "index.json")
        with open(top_index_path, "w", encoding="utf-8") as jf:
            json.dump(top_index, jf, indent=4, ensure_ascii=False)
        print(
            f"\nWrote index file: {top_index_path} "
            f"({len(top_index)} merged folders)"
        )


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python merge.py <root_folder>")
        sys.exit(1)

    root = sys.argv[1]
    merge_files_in_subfolders(root)
