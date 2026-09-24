#!/usr/bin/env bash
#
# Cut a CyThing release: one version number into the three places that carry
# it, one commit, one annotated tag, pushed. See doc/git-workflow.md "Release".
#
#   scripts/release.sh 1.1.0
#
# Updates
#   library.properties            version=1.1.0          (Arduino Library Manager)
#   library.json                  "version": "1.1.0"     (PlatformIO)
#   src/device_config/device_config.h  FIRMWARE_VERSION   (what the device reports
#                                                          in GET_INFO / MQTT get-info)
# then commits "Release 1.1.0", tags 1.1.0 and pushes main + the tag.
# Refuses to run on a dirty tree, off main, or if the tag already exists.
set -euo pipefail

VERSION="${1:-}"
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "usage: $0 MAJOR.MINOR.PATCH   (e.g. 1.1.0)" >&2
    exit 2
fi

cd "$(git rev-parse --show-toplevel)"

if [[ "$(git branch --show-current)" != "main" ]]; then
    echo "release.sh: switch to main first (releases are cut from merged main)" >&2
    exit 1
fi
if [[ -n "$(git status --porcelain)" ]]; then
    echo "release.sh: working tree is not clean — commit or stash first" >&2
    exit 1
fi
git fetch -q origin
if [[ "$(git rev-parse HEAD)" != "$(git rev-parse origin/main)" ]]; then
    echo "release.sh: local main differs from origin/main — pull/push first" >&2
    exit 1
fi
if git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null || git ls-remote --tags origin "refs/tags/$VERSION" | grep -q .; then
    echo "release.sh: tag $VERSION already exists" >&2
    exit 1
fi

# sed -i with a backup suffix works the same on macOS and GNU sed.
sed -i.bak "s/^version=.*/version=$VERSION/" library.properties
sed -i.bak "s/\"version\": *\"[^\"]*\"/\"version\": \"$VERSION\"/" library.json
sed -i.bak "s/^#define FIRMWARE_VERSION .*/#define FIRMWARE_VERSION \"$VERSION\"/" src/device_config/device_config.h
rm -f library.properties.bak library.json.bak src/device_config/device_config.h.bak

grep -q "^version=$VERSION$" library.properties
grep -q "\"version\": \"$VERSION\"" library.json
grep -q "^#define FIRMWARE_VERSION \"$VERSION\"$" src/device_config/device_config.h

git add library.properties library.json src/device_config/device_config.h
if git diff --cached --quiet; then
    echo "release.sh: all three files already say $VERSION; tagging HEAD as is"
else
    git commit -q -m "Release $VERSION"
fi
git tag -a "$VERSION" -m "CyThing $VERSION"
git push -q origin main "$VERSION"

echo "Released $VERSION: $(git rev-parse --short HEAD) tagged and pushed."
echo "  Arduino ZIP:  gh release create $VERSION --generate-notes   (optional)"
echo "  PlatformIO:   lib_deps = https://github.com/mbahmani90/cything.git#$VERSION"
