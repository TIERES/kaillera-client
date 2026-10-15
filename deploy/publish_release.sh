#!/usr/bin/env bash
# Publishes a kailleraclient GitHub release to the community server's
# self-updater (see common/n02_update.cpp / wg-camp's app/updates.py), plus
# the RetroArch TIERES package the DLL updates inside the TIERES RetroArch:
#
#  1. downloads both platform zips of the release and extracts each
#     kailleraclient.dll under a distinct name (the DLL-only update);
#  2. takes the last light zip the fork's pkg/tieres/empacotar.ps1 generated
#     (it writes the path to %LOCALAPPDATA%\RetroArch-TIERES\ultima-light.txt),
#     checks that its kailleraclient.dll is this release, and builds the
#     package manifest (sha256/size/path of every file in it);
#  3. compares it with the manifest the site serves now and uploads only the
#     files the site doesn't have yet - no zip, nothing to unpack server-side;
#  4. runs publish_pacote.sh and publish_update.sh on the server.
#
# Run via the "Publish release to updater" VS Code task, or directly:
#
#   deploy/publish_release.sh [tag] [--light <zip>] [--so-dll] [--simular]
#
#   tag        defaults to the latest published release
#   --light    this light zip instead of the last one empacotar.ps1 generated
#   --so-dll   the DLL only, as before the package existed
#   --simular  everything up to the comparison, uploads nothing
set -euo pipefail

REPO="TIERES/kaillera-client"
SERVER_HOST="kaillera"
REMOTE_SCRIPT="/opt/arena17-downloads/deploy/publish_update.sh"
REMOTE_PACOTE="/opt/arena17-downloads/deploy/publish_pacote.sh"
SITE="${SITE:-https://we2002.wgs.dev.br}"
ULTIMA_LIGHT="$(cygpath -u "${LOCALAPPDATA:-}" 2>/dev/null || echo "${LOCALAPPDATA:-}")/RetroArch-TIERES/ultima-light.txt"

VERSION=""
LIGHT=""
SO_DLL=0
SIMULAR=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --light) LIGHT="${2:-}"; shift 2 ;;
        --so-dll) SO_DLL=1; shift ;;
        --simular) SIMULAR=1; shift ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) VERSION="$1"; shift ;;
    esac
done

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

# The version a kailleraclient.dll was built as: N02_WINDOW_TITLE, "N02 <version>".
dll_version() {
    LC_ALL=C grep -aoE 'N02 (v[A-Za-z0-9.+_-]+|dev)' "$1" | head -1 | cut -c5-
}

if [ "$(dll_version "$WORKDIR/kailleraclient-x64.dll")" != "$VERSION" ]; then
    echo "The release's x64 DLL says '$(dll_version "$WORKDIR/kailleraclient-x64.dll")', not $VERSION." >&2
    exit 1
fi

PACOTE_NOVOS="$WORKDIR/pacote-novos"
mkdir -p "$PACOTE_NOVOS"

if [ "$SO_DLL" = 0 ]; then
    echo "-- RetroArch TIERES package (light)..."
    if [ -z "$LIGHT" ]; then
        if [ ! -f "$ULTIMA_LIGHT" ]; then
            echo "Couldn't find the last light zip: $ULTIMA_LIGHT doesn't exist." >&2
            echo "Generate the light with pkg/tieres/empacotar.ps1 (retroarch-k3-ffw), pass --light <zip>," >&2
            echo "or --so-dll to publish only the DLL." >&2
            exit 1
        fi
        LIGHT=$(tr -d '\r\n' < "$ULTIMA_LIGHT")
    fi
    LIGHT=$(cygpath -u "$LIGHT" 2>/dev/null || echo "$LIGHT")
    if [ ! -f "$LIGHT" ]; then
        echo "Light zip not found: $LIGHT" >&2
        exit 1
    fi
    echo "   $LIGHT ($(date -r "$LIGHT" '+%d/%m/%Y %H:%M'))"

    LIGHT_DIR="$WORKDIR/light"
    mkdir -p "$LIGHT_DIR"
    unzip -o -q "$LIGHT" -d "$LIGHT_DIR"

    LIGHT_DLL_VERSION=$(dll_version "$LIGHT_DIR/kailleraclient.dll" 2>/dev/null || true)
    if [ "$LIGHT_DLL_VERSION" != "$VERSION" ]; then
        echo "The light's kailleraclient.dll is '${LIGHT_DLL_VERSION:-?}', not $VERSION." >&2
        echo "Rebuild the light with this release's DLL (empacotar.ps1), or use --so-dll." >&2
        exit 1
    fi
    if ! cmp -s "$LIGHT_DIR/kailleraclient.dll" "$WORKDIR/kailleraclient-x64.dll"; then
        echo "   note: the light's DLL is a different build of $VERSION than the release's (same version, so no update loop)."
    fi

    # Same rule the DLL enforces: binaries only - never a player's settings.
    PACOTE_NOME=$(basename "$LIGHT" .light.zip)
    MANIFESTO="$WORKDIR/x64.txt"
    printf 'versao\t%s\npacote\t%s\n' "$VERSION" "$PACOTE_NOME" > "$MANIFESTO"
    permitido='^[A-Za-z0-9._ /-]+\.(exe|dll|lck)$'
    while IFS= read -r arquivo; do
        caminho=${arquivo#"$LIGHT_DIR"/}
        if ! [[ "$caminho" =~ $permitido ]]; then
            echo "The light has '$caminho' - only .exe/.dll/.lck can go in the package." >&2
            exit 1
        fi
        sha=$(sha256sum "$arquivo" | cut -d' ' -f1)
        printf 'arquivo\t%s\t%s\t%s\n' "$sha" "$(stat -c %s "$arquivo")" "$caminho" >> "$MANIFESTO"
    done < <(find "$LIGHT_DIR" -type f | LC_ALL=C sort)
    grep -q $'\tretroarch.exe$' "$MANIFESTO" || { echo "The light has no retroarch.exe." >&2; exit 1; }

    # What the site has now: a file already in its manifest is already there.
    ATUAL="$WORKDIR/atual.txt"
    if ! curl -fsS "$SITE/updates/pacote/x64.txt" -o "$ATUAL" 2>/dev/null; then
        : > "$ATUAL"
        echo "   (the site has no package yet)"
    else
        echo "   site now: $(awk -F'\t' '$1 == "versao" { print $2 }' "$ATUAL") ($(awk -F'\t' '$1 == "pacote" { print $2 }' "$ATUAL"))"
    fi
    total_novo=0
    while IFS=$'\t' read -r tipo sha tamanho caminho; do
        [ "$tipo" = arquivo ] || continue
        if awk -F'\t' -v s="$sha" '$1 == "arquivo" && $2 == s { found = 1 } END { exit !found }' "$ATUAL"; then
            estado="same as the site"
        else
            estado="NEW - uploading"
            cp "$LIGHT_DIR/$caminho" "$PACOTE_NOVOS/$sha"
            total_novo=$((total_novo + tamanho))
        fi
        printf '   %-40s %10s bytes  %s\n' "$caminho" "$tamanho" "$estado"
    done < "$MANIFESTO"
    echo "   to upload: $((total_novo / 1024)) KB"
fi

if [ "$SIMULAR" = 1 ]; then
    echo "== Simulation: nothing uploaded =="
    [ "$SO_DLL" = 0 ] && cat "$MANIFESTO"
    exit 0
fi

REMOTE_TMP="/tmp/publish-$VERSION-$$"
echo "-- Uploading to $SERVER_HOST..."
ssh "$SERVER_HOST" "mkdir -p '$REMOTE_TMP/pacote-novos'"
scp -q "$WORKDIR/kailleraclient-x64.dll" "$WORKDIR/kailleraclient-x86.dll" "$SERVER_HOST:$REMOTE_TMP/"
if [ "$SO_DLL" = 0 ]; then
    scp -q "$MANIFESTO" "$SERVER_HOST:$REMOTE_TMP/"
    if [ -n "$(ls -A "$PACOTE_NOVOS")" ]; then
        scp -q "$PACOTE_NOVOS"/* "$SERVER_HOST:$REMOTE_TMP/pacote-novos/"
    fi
fi

echo "-- Publishing on server..."
PUBLICAR="sudo bash '$REMOTE_SCRIPT' '$VERSION' '$REMOTE_TMP/kailleraclient-x64.dll' '$REMOTE_TMP/kailleraclient-x86.dll'"
if [ "$SO_DLL" = 0 ]; then
    # package first: if it fails, nothing changed for anyone
    PUBLICAR="sudo bash '$REMOTE_PACOTE' '$REMOTE_TMP/x64.txt' '$REMOTE_TMP/pacote-novos' && $PUBLICAR"
fi
ssh "$SERVER_HOST" "$PUBLICAR; status=\$?; rm -rf '$REMOTE_TMP'; exit \$status"

if [ "$SO_DLL" = 0 ]; then
    echo "-- Checking what the site serves..."
    if curl -fsS "$SITE/updates/pacote/x64.txt" | cmp -s - "$MANIFESTO"; then
        echo "   package manifest OK"
    else
        echo "   WARNING: the site's package manifest differs from the one just published." >&2
        exit 1
    fi
fi

echo "== Published $VERSION =="
