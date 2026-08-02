#!/bin/bash
# commit.sh — commit to linux-build-env then sync main
# Usage: ./commit.sh "commit message"

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 \"commit message\""
    exit 1
fi

MSG="$1"

# Must be on linux-build-env
BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$BRANCH" != "linux-build-env" ]; then
    echo "ERROR: not on linux-build-env (currently on $BRANCH)"
    exit 1
fi

# Stage all tracked changes + new files in known source dirs
git add -A

git commit -m "$MSG"
git push origin linux-build-env

# Merge into main
git checkout main
git merge linux-build-env -X theirs --no-edit
git push origin main

# Return to working branch
git checkout linux-build-env

echo ""
echo "Done. Both branches updated."
