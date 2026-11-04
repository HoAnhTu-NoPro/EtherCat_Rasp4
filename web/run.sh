#!/bin/sh
# Chạy web server (không cần root). User phải thuộc nhóm [ipc] group trong axis.ini.
cd "$(dirname "$0")"
CFG="${ECAT_WEB_CONFIG:-../config/web.ini}"
HOST=$(sed -n 's/^host *= *//p' "$CFG")
PORT=$(sed -n 's/^port *= *//p' "$CFG")
exec .venv/bin/uvicorn server.app:app --host "${HOST:-0.0.0.0}" --port "${PORT:-8080}" \
  --no-access-log --ws-ping-interval 5 --ws-ping-timeout 5
