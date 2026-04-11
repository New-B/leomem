#!/usr/bin/env python3
import csv
import pathlib
import sys


def parse_csv_line(text: str):
    for line in text.splitlines():
        if not line.startswith("csv: "):
            continue
        fields = {}
        for item in line[5:].split(","):
            if "=" not in item:
                continue
            key, value = item.split("=", 1)
            fields[key] = value
        return fields
    return None


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: collect_results.py <results-dir>", file=sys.stderr)
        return 1

    results_dir = pathlib.Path(sys.argv[1])
    rows = []
    columns = ["config"]
    for path in sorted(results_dir.glob("*.txt")):
        parsed = parse_csv_line(path.read_text())
        if parsed is None:
            continue
        parsed["config"] = path.stem
        rows.append(parsed)
        for key in parsed.keys():
            if key not in columns:
                columns.append(key)

    if not rows:
        return 0

    writer = csv.DictWriter(sys.stdout, fieldnames=columns)
    writer.writeheader()
    for row in rows:
        writer.writerow(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
