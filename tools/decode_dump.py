"""Convert the output of the CLI `dump` command into a clean CSV file.

Capture the serial output of `dump` to a text file (e.g. with PuTTY logging
or `commander-cli` VCOM), then:

    python decode_dump.py capture.txt -o measurements.csv

Lines outside the CSV block (banners, log lines) are ignored.
"""
import argparse
import csv
import sys

HEADER = "seq,boot_id,uptime_s,laeq_db,lafmax_db"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", help="text file with the captured dump output")
    parser.add_argument("-o", "--output", default="measurements.csv")
    args = parser.parse_args()

    rows = []
    in_block = False
    with open(args.capture, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line == HEADER:
                in_block = True
                continue
            if not in_block:
                continue
            if line == "end":
                break
            parts = line.split(",")
            if len(parts) == 5:
                rows.append(parts)

    if not rows:
        print("nenhum registro encontrado no arquivo", file=sys.stderr)
        return 1

    with open(args.output, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(HEADER.split(","))
        writer.writerows(rows)

    print(f"{len(rows)} registros salvos em {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
