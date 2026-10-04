#!/usr/bin/env bash
# Start the QuickFlip server on this PC (gallery, drawing app and upload API on port 8780).
#   bash run.sh               start it
#   bash run.sh --port 9000   on another port
cd "$(dirname "$0")"
exec python3 server/server.py "$@"
