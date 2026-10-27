#!/usr/bin/env sh
set -eu
backend_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$backend_dir/server.py" serve "$@"
