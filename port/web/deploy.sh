#!/bin/bash
# deploy.sh [site folder] [maps folder]: copies the browser build (ninja web)
# to the site nginx serves (port/web/nginx.conf), and lists the maps for the
# pages (maps/index.json).
#
# The engine's files get the build's name (halo.<build>.js and .wasm), and the
# last few builds stay: the game's threads load halo.js again as they start,
# so a page opened before a deploy must still find its own build's files.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SITE=${1:-/srv/halo/site}
MAPS=${2:-/srv/halo/maps}
KEEP=5
BUILD_DIR="$ROOT/build/web/site"
BUILD=$(cat "$BUILD_DIR/halo.js" "$BUILD_DIR/halo.wasm" | sha256sum | cut -c1-12)
sudo mkdir -p "$SITE"
# the page and its scripts, as built (but not the engine's unnamed files,
# nor the engines of earlier builds)
sudo rsync -a --delete --exclude 'halo.js' --exclude 'halo.wasm' --exclude 'halo.*.js' --exclude 'halo.*.wasm' \
	"$BUILD_DIR/" "$SITE/"
sudo cp "$BUILD_DIR/halo.js" "$SITE/halo.$BUILD.js"
sudo cp "$BUILD_DIR/halo.wasm" "$SITE/halo.$BUILD.wasm"
# the page loads this build's engine
sudo sed -i "s#<script src=\"page.js\"></script>#<script>window.HALO_BUILD = '$BUILD';</script><script src=\"page.js?$BUILD\"></script>#" \
	"$SITE/index.html"
# the oldest engines go
ls -t "$SITE"/halo.*.wasm | tail -n +$((KEEP + 1)) | while read -r old; do
	sudo rm -f "$old" "${old%.wasm}.js"
done
python3 - "$MAPS" <<PY | sudo tee "$MAPS/index.json" > /dev/null
import json, sys
from pathlib import Path
files = [{"name": p.name, "size": p.stat().st_size} for p in sorted(Path(sys.argv[1]).glob("*.map"))]
print(json.dumps({"files": files}))
PY
echo "deployed build $BUILD to $SITE"
