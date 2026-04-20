#!/usr/bin/env bash
# Sync acecode's bundled models.dev snapshot by fetching the published api.json
# directly. Much simpler than mirroring the upstream repo + re-running its build:
# models.dev already ships the compiled JSON at https://models.dev/api.json, and
# that URL is what acecode's runtime network-refresh path hits too.
#
# Usage:
#   scripts/sync_models_dev.sh [--url <override>]
#
# Exits non-zero on: HTTP failure, JSON parse failure, or sanity-check failure
# (<50 providers or <1000 models).

set -euo pipefail

SCRIPT_VERSION=2
URL="https://models.dev/api.json"

while [ $# -gt 0 ]; do
    case "$1" in
        --url) URL="$2"; shift 2 ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$REPO_ROOT/assets/models_dev"
mkdir -p "$DEST"

TMP="$(mktemp -t models_dev_api.XXXXXX.json)"
trap 'rm -f "$TMP"' EXIT

echo "Fetching $URL..."
curl -fsSL --retry 3 --connect-timeout 10 -o "$TMP" "$URL"

python3 - "$TMP" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
if not isinstance(data, dict):
    sys.exit("top-level not an object")
providers = sum(1 for v in data.values() if isinstance(v, dict))
models = 0
for p in data.values():
    if not isinstance(p, dict): continue
    m = p.get("models")
    if isinstance(m, dict): models += len(m)
    elif isinstance(m, list): models += len(m)
if providers < 50:
    sys.exit(f"provider count too low: {providers}")
if models < 1000:
    sys.exit(f"model count too low: {models}")
print(f"validated: providers={providers} models={models}")
PY

PROVIDER_COUNT=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1], encoding="utf-8")); print(sum(1 for v in d.values() if isinstance(v,dict)))' "$TMP")
MODEL_COUNT=$(python3 -c 'import json,sys
d=json.load(open(sys.argv[1], encoding="utf-8"))
n=0
for v in d.values():
    if not isinstance(v,dict): continue
    m=v.get("models")
    if isinstance(m,dict): n+=len(m)
    elif isinstance(m,list): n+=len(m)
print(n)' "$TMP")

# Compact canonical form (UTF-8, LF, no BOM) so git diff stays stable.
python3 -c 'import json,sys; json.dump(json.load(open(sys.argv[1], encoding="utf-8")), open(sys.argv[2],"w",encoding="utf-8",newline="\n"), separators=(",",":"))' "$TMP" "$DEST/api.json"

CONTENT_SHA=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$DEST/api.json")
GENERATED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)

python3 - "$DEST/MANIFEST.json" <<PY
import json, sys
manifest = {
    "upstream_remote": "$URL",
    "content_sha256": "$CONTENT_SHA",
    "generated_at": "$GENERATED_AT",
    "provider_count": $PROVIDER_COUNT,
    "model_count": $MODEL_COUNT,
    "acecode_sync_script_version": $SCRIPT_VERSION,
}
with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY

# LICENSE: the published api.json has no accompanying license payload. Keep the
# committed LICENSE file (which points at the upstream MIT text) untouched so
# attribution is stable and doesn't churn with each sync. To refresh it, fetch
# https://raw.githubusercontent.com/anomalyco/models.dev/dev/LICENSE manually.

echo "Sync OK: providers=$PROVIDER_COUNT models=$MODEL_COUNT sha256=${CONTENT_SHA:0:12}..."
echo "  Wrote $DEST/api.json"
echo "  Wrote $DEST/MANIFEST.json"
