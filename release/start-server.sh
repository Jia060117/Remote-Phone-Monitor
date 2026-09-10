#!/usr/bin/env sh
set -eu

RELEASE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SERVER_ROOT="$RELEASE_ROOT/server"

if [ ! -f "$SERVER_ROOT/server.json" ] || [ ! -f "$SERVER_ROOT/tokens.json" ]; then
    echo "Run: python3 initialize.py --server-url https://your-server.example" >&2
    exit 1
fi

cd "$SERVER_ROOT"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
exec .venv/bin/python -m uvicorn main:app --host 0.0.0.0 --port 8000
