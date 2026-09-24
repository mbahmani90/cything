#!/usr/bin/env bash
#
# Package the cything ESP-IDF component as a self-contained archive for the
# Espressif Component Registry.
#
# components/cything/CMakeLists.txt points at ../../src on purpose, so the
# same src/ tree Arduino/PlatformIO compile directly is also what an
# EXTRA_COMPONENT_DIRS/git-submodule ESP-IDF consumer gets — one copy of the
# source, never two. The registry instead distributes each component as a
# standalone archive: it packs whatever is under --project-dir, nothing
# outside it, so pointing that straight at components/cything would produce
# an archive with the two build files and none of the actual source.
#
# This script works around that without touching the committed layout: it
# assembles a throwaway, self-contained copy (src/ copied in, the CMakeLists
# rewritten to a local path) in a scratch directory, packs *that*, and
# deletes the scratch copy. Nothing here is committed to the repo.
#
#   scripts/pack_component.sh [VERSION]
#
# VERSION defaults to library.properties's version (kept in sync with
# library.json / FIRMWARE_VERSION by scripts/release.sh). Output:
# dist/pack/cything_<version>.tgz (dist/ is git-ignored — scratch only).
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

VERSION="${1:-$(sed -n 's/^version=//p' library.properties)}"
if [[ -z "$VERSION" ]]; then
    echo "pack_component.sh: no version given and none found in library.properties" >&2
    exit 1
fi

# compote ships inside each IDF install's own Python env, not as a separate
# tool. Point IDF_PY_ENV at one if the default doesn't match your setup.
IDF_PY_ENV="${IDF_PY_ENV:-$HOME/.espressif/python_env/idf6.0_py3.14_env}"
COMPOTE="$IDF_PY_ENV/bin/compote"
if [[ ! -x "$COMPOTE" ]]; then
    echo "pack_component.sh: compote not found at $COMPOTE (set IDF_PY_ENV to an ESP-IDF Python env directory)" >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp -R src "$STAGE/src"
cp components/cything/Kconfig.projbuild "$STAGE/Kconfig.projbuild"
cp components/cything/idf_component.yml "$STAGE/idf_component.yml"
cp -R components/cything/examples "$STAGE/examples"
sed 's#\${CMAKE_CURRENT_LIST_DIR}/../../src#${CMAKE_CURRENT_LIST_DIR}/src#' \
    components/cything/CMakeLists.txt > "$STAGE/CMakeLists.txt"
grep -q '\${CMAKE_CURRENT_LIST_DIR}/src' "$STAGE/CMakeLists.txt" || {
    echo "pack_component.sh: path rewrite didn't match anything — components/cything/CMakeLists.txt changed shape, update the sed pattern above" >&2
    exit 1
}

DEST="$(pwd)/dist/pack"
mkdir -p "$DEST"
"$COMPOTE" component pack --project-dir "$STAGE" --name cything --version "$VERSION" --dest-dir "$DEST"

echo "Packed: $DEST/cything_$VERSION.tgz"
