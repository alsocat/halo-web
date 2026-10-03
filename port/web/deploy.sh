#!/bin/bash
# deploy.sh [site folder] [maps folder]: copies the browser build (ninja web)
# to the site nginx serves (port/web/nginx.conf), and lists the maps for the
# pages (maps/index.json).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SITE=${1:-/srv/halo/site}
MAPS=${2:-/srv/halo/maps}
sudo mkdir -p "$SITE"
sudo rsync -a --delete "$ROOT/build/web/site/" "$SITE/"
python3 - "$MAPS" <<PY | sudo tee "$MAPS/index.json" > /dev/null
import json, sys
from pathlib import Path
files = [{"name": p.name, "size": p.stat().st_size} for p in sorted(Path(sys.argv[1]).glob("*.map"))]
print(json.dumps({"files": files}))
PY
echo "deployed to $SITE"
