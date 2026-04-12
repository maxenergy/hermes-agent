#!/bin/bash
set -euo pipefail
# Usage: ./release.sh patch|minor|major
BUMP=${1:-patch}
cd "$(dirname "$0")/.."

# Parse current version from CHANGELOG (first ## [X.Y.Z] header).
CURRENT=$(grep -oP '## \[\K[0-9]+\.[0-9]+\.[0-9]+' CHANGELOG.md | head -1)
if [ -z "$CURRENT" ]; then
    echo "ERROR: could not parse current version from CHANGELOG.md" >&2
    exit 1
fi

MAJOR=$(echo "$CURRENT" | cut -d. -f1)
MINOR=$(echo "$CURRENT" | cut -d. -f2)
PATCH=$(echo "$CURRENT" | cut -d. -f3)

case $BUMP in
    major) NEW="$((MAJOR+1)).0.0" ;;
    minor) NEW="$MAJOR.$((MINOR+1)).0" ;;
    patch) NEW="$MAJOR.$MINOR.$((PATCH+1))" ;;
    *) echo "Usage: $0 patch|minor|major" >&2; exit 1 ;;
esac

echo "Bumping $CURRENT -> $NEW"

# Update version in CMakeLists.txt
sed -i "s/VERSION $CURRENT/VERSION $NEW/" CMakeLists.txt

# Prepend a new CHANGELOG entry.
DATE=$(date +%Y-%m-%d)
sed -i "0,/^## /s//## [$NEW] - $DATE\n\n### Added\n- TBD\n\n## /" CHANGELOG.md

git add CMakeLists.txt CHANGELOG.md
git commit -m "chore: release v$NEW"
git tag -a "v$NEW" -m "Release v$NEW"

echo "Tagged v$NEW. Push with: git push --follow-tags"
