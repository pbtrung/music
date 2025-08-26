import sys
import json
import os


def merge_ids(index_path, file_id_path, output_path):
    # Check if output file already exists
    if os.path.exists(output_path):
        print(f"Error: output file '{output_path}' already exists. Aborting.")
        sys.exit(1)

    # Load index.json
    with open(index_path, "r", encoding="utf-8") as f:
        index_data = json.load(f)

    # Load file_id.json (list of objects with Name + ID)
    with open(file_id_path, "r", encoding="utf-8") as f:
        file_id_data = json.load(f)

    # Build lookup: Name -> ID
    id_map = {entry["Name"]: entry["ID"] for entry in file_id_data}

    # Merge ID into index.json where merged_file.name matches Name
    for entry in index_data:
        merged_name = entry.get("merged_file", {}).get("name")
        if merged_name:
            if merged_name in id_map:
                entry["merged_file"]["file_id"] = id_map[merged_name]
            else:
                print(
                    f"Error: no matching file_id found for '{merged_name}'. Aborting."
                )
                sys.exit(1)

    # Write output
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(index_data, f, indent=4, ensure_ascii=False)

    print(f"Merged IDs written to {output_path}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: python merge_with_id.py index.json file_id.json output.json")
        sys.exit(1)

    merge_ids(sys.argv[1], sys.argv[2], sys.argv[3])
