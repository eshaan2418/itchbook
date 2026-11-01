#!/usr/bin/env bash
# Download a public NASDAQ TotalView-ITCH 5.0 sample file and verify its MD5.
# Source: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ (NASDAQ public sample data).
# The file is kept compressed; all tools in this repo stream-decompress it.
# NASDAQ's .md5sum links currently return 404, so SHA-256 digests are pinned here (computed on
# first download, cross-checked by a full gzip decode during replay).
set -euo pipefail
DAY="${1:-01302019}"
DEST="${ITCH_DATA_DIR:-$HOME/quantfactory/_data/itch}"
BASE="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
FILE="${DAY}.NASDAQ_ITCH50.gz"
mkdir -p "$DEST"
cd "$DEST"
pinned_sha256() {  # bash 3.2 (macOS) has no associative arrays
  case "$1" in
    01302019) echo 8c97b5b13bc451c012c2466fb7e258da134dab29aa47b67fe7b0088c78e870be ;;
    *) echo "" ;;
  esac
}
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi; }
if [[ ! -f "$FILE" ]]; then
  echo "Downloading $FILE (~4.7 GB) ..."
  curl -fL --retry 5 --retry-delay 10 -C - -o "$FILE.part" "${BASE}/${FILE}"
  mv "$FILE.part" "$FILE"
fi
actual=$(sha "$FILE")
expected=$(pinned_sha256 "$DAY")
if [[ -z "$expected" ]]; then
  echo "WARNING: no pinned digest for $DAY; sha256=$actual" >&2
elif [[ "$expected" != "$actual" ]]; then
  echo "SHA-256 mismatch: expected $expected got $actual" >&2; exit 1
fi
echo "OK $DEST/$FILE ($actual)"
