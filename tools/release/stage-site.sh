#!/usr/bin/env bash
# Stages the public AudioX website into OUTPUT (served at /audiox/):
#   download page, shared release scripts and branding, VitePress docs (docs/),
#   and the Live Demo build of the device web UI (demo/).
# --static only copies the download page files (used by mirror-updates.sh).
set -euo pipefail

STATIC_ONLY=0
if [[ "${1:-}" == --static ]]; then
    STATIC_ONLY=1
    shift
fi
OUTPUT="${1:?usage: stage-site.sh [--static] <output_directory>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

mkdir -p "$OUTPUT"
cp "$ROOT/web/public/index.html" "$ROOT/web/public/downloads.js" "$ROOT/web/public/downloads.css" \
    "$ROOT/web/embedded/styles.css" "$ROOT/web/embedded/releases.js" "$ROOT/assets/branding/logo.svg" \
    "$ROOT/assets/branding/logo_small.png" "$OUTPUT/"
touch "$OUTPUT/.nojekyll"
[[ "$STATIC_ONLY" == 1 ]] && exit 0

npm ci --prefix "$ROOT/web/embedded" --ignore-scripts --no-audit --no-fund
node "$ROOT/web/embedded/build.js" --target demo --out "$OUTPUT/demo"

npm ci --prefix "$ROOT/docs" --no-audit --no-fund
npm run docs:build --prefix "$ROOT/docs"
rm -rf "$OUTPUT/docs"
mkdir -p "$OUTPUT/docs"
cp -R "$ROOT/docs/.vitepress/dist/." "$OUTPUT/docs/"
