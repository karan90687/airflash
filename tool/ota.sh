#!/usr/bin/env bash
# tool/ota.sh flash <project_dir>   build that ESP-IDF project + upload its app over Wi-Fi
# tool/ota.sh logs                  tail device logs
# env: HOST (default esp32.local), TOKEN (if set in menuconfig), PORT (default 80)
set -euo pipefail
HOST=${HOST:-esp32.local}
URL="http://$HOST:${PORT:-80}"
AUTH=(-H "X-Token: ${TOKEN:-}")

case "${1:-}" in
  flash)
    cd "${2:?usage: $0 flash <project_dir>}"   # required: never upload the wrong project by accident
    idf.py build
    bin=$(python3 -c 'import json;d=json.load(open("build/project_description.json"));print(d["build_dir"]+"/"+d["app_bin"])')
    echo "uploading $bin ($(wc -c < "$bin" | tr -d ' ') bytes) to $URL"
    curl --fail-with-body -sS --data-binary @"$bin" \
      -H "Content-Type: application/octet-stream" "${AUTH[@]}" "$URL/update"
    ;;
  logs)
    from=0; hdr=$(mktemp); trap 'rm -f "$hdr"' EXIT
    while :; do
      if curl -s -m 5 -D "$hdr" "${AUTH[@]}" "$URL/logs?from=$from"; then
        from=$(awk -F': ' 'tolower($1)=="x-next"{print $2+0}' "$hdr")
        from=${from:-0}
      fi
      sleep 0.5
    done
    ;;
  *) echo "usage: $0 flash <project_dir> | logs"; exit 1 ;;
esac
