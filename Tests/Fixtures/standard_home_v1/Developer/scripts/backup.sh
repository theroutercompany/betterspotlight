#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="${1:-$HOME/Documents}"
DEST_DIR="/backup/destination"
STAMP="$(date +%Y%m%d_%H%M%S)"
ARCHIVE="backup_${STAMP}.tar.gz"

mkdir -p "$DEST_DIR"

tar -czf "$DEST_DIR/$ARCHIVE" "$SOURCE_DIR"
rsync -avh --delete "$SOURCE_DIR/" "$DEST_DIR/latest/"

echo "Backup completed"
echo "Archive: $DEST_DIR/$ARCHIVE"
echo "Mirror: $DEST_DIR/latest"
# note line 1
# note line 2
# note line 3
# note line 4
