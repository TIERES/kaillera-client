#!/usr/bin/env bash
# Publishes a kailleraclient GitHub release to the community server's
# self-updater (see common/n02_update.cpp / wg-camp's app/updates.py):
# downloads both platform zips, extracts each kailleraclient.dll under a
# distinct name, uploads them to the server, and runs publish_update.sh
# there. Run via the "Publish release to updater" VS Code task, or
# directly: deploy/publish_release.sh [tag]  (tag defaults to the latest
# published release).
set -euo pipefail

REPO="TIERES/kaillera-client"
SERVER_HOST="kaillera"
REMOTE_SCRIPT="/opt/arena17-downloads/deploy/publish_update.sh"

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    VERSION=$(gh release list --repo "$REPO" --limit 1 | cut -f3)
fi
if [ -z "$VERSION" ]; then
    echo "Could not determine a release tag to publish (gh release list returned nothing)." >&2
    exit 1
fi

echo "== Publishing $VERSION to the self-updater =="

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

echo "-- Downloading release assets..."
gh release download "$VERSION" --repo "$REPO" --dir "$WORKDIR" --clobber \
    -p "kailleraclient-x64-*.zip" -p "kailleraclient-x86-*.zip"

echo "-- Extracting..."
mkdir -p "$WORKDIR/x64" "$WORKDIR/x86"
unzip -o -q "$WORKDIR"/kailleraclient-x64-*.zip -d "$WORKDIR/x64"
unzip -o -q "$WORKDIR"/kailleraclient-x86-*.zip -d "$WORKDIR/x86"
mv "$WORKDIR/x64/kailleraclient.dll" "$WORKDIR/kailleraclient-x64.dll"
mv "$WORKDIR/x86/kailleraclient.dll" "$WORKDIR/kailleraclient-x86.dll"

echo "-- Uploading to $SERVER_HOST..."
scp "$WORKDIR/kailleraclient-x64.dll" "$WORKDIR/kailleraclient-x86.dll" "$SERVER_HOST:/tmp/"

echo "-- Publishing on server..."
ssh "$SERVER_HOST" "sudo bash '$REMOTE_SCRIPT' '$VERSION' /tmp/kailleraclient-x64.dll /tmp/kailleraclient-x86.dll && rm -f /tmp/kailleraclient-x64.dll /tmp/kailleraclient-x86.dll"

echo "== Published $VERSION =="
