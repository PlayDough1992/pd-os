#!/bin/bash
# ============================================================================
# sync-asm.sh — Commit and push PD-OS source files to both branches
# ============================================================================
# Usage: ./sync-asm.sh "commit message"
#
# Syncs all OS-independent source files:
#   *.asm  *.c  *.h  *.ld
#
# Excluded (host-OS specific):
#   *.sh   *.ps1  Makefile  build.sh  build.ps1
#
# This script:
#   1. Stages all modified/new source files matching the patterns above
#   2. Commits them on the current branch
#   3. Pushes the current branch
#   4. Cherry-picks the commit onto the other branch (main <-> linux-build-env)
#   5. Pushes the other branch
#   6. Returns you to your original branch
# ============================================================================

set -e

BRANCH_A="main"
BRANCH_B="linux-build-env"

# Source file patterns to sync (not host-OS specific)
SYNC_PATTERNS=("*.asm" "*.c" "*.h" "*.ld")

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Require a commit message
if [ -z "$1" ]; then
    echo -e "${RED}Error: Please provide a commit message.${NC}"
    echo "Usage: ./sync-asm.sh \"your commit message\""
    exit 1
fi

COMMIT_MSG="$1"
ORIGINAL_BRANCH=$(git rev-parse --abbrev-ref HEAD)

# Determine the target (other) branch
if [ "$ORIGINAL_BRANCH" = "$BRANCH_A" ]; then
    OTHER_BRANCH="$BRANCH_B"
elif [ "$ORIGINAL_BRANCH" = "$BRANCH_B" ]; then
    OTHER_BRANCH="$BRANCH_A"
else
    echo -e "${RED}Error: You are on branch '$ORIGINAL_BRANCH'.${NC}"
    echo "This script only syncs between '$BRANCH_A' and '$BRANCH_B'."
    echo "Switch to one of those branches first."
    exit 1
fi

# Check for source files with changes (modified, new, or deleted)
CHANGED_FILES=""
for pat in "${SYNC_PATTERNS[@]}"; do
    CHANGED_FILES+=$(git diff --name-only HEAD -- "$pat")$'\n'
    CHANGED_FILES+=$(git ls-files --others --exclude-standard -- "$pat")$'\n'
done
CHANGED_FILES=$(echo "$CHANGED_FILES" | sed '/^$/d')

if [ -z "$CHANGED_FILES" ] && ! git diff --cached --name-only -- "${SYNC_PATTERNS[@]}" | grep -q '.'; then
    echo -e "${YELLOW}No source file changes detected.${NC}"
    exit 0
fi

echo -e "${CYAN}Source files to sync:${NC}"
echo "$CHANGED_FILES"
echo ""

# Stage the matching source files
for pat in "${SYNC_PATTERNS[@]}"; do
    git add -- "$pat" 2>/dev/null || true
done

# Verify something was staged
STAGED=$(git diff --cached --name-only -- "${SYNC_PATTERNS[@]}")
if [ -z "$STAGED" ]; then
    echo -e "${YELLOW}No source changes to commit.${NC}"
    exit 0
fi

# Commit on the current branch
echo -e "${CYAN}[$ORIGINAL_BRANCH] Committing .asm changes...${NC}"
git commit -m "$COMMIT_MSG"
COMMIT_HASH=$(git rev-parse HEAD)
echo -e "${GREEN}[OK] Committed: $COMMIT_HASH${NC}"

# Push current branch
echo -e "${CYAN}[$ORIGINAL_BRANCH] Pushing...${NC}"
git push origin "$ORIGINAL_BRANCH"
echo -e "${GREEN}[OK] Pushed $ORIGINAL_BRANCH${NC}"

# Switch to other branch and cherry-pick
echo ""
echo -e "${CYAN}[$OTHER_BRANCH] Cherry-picking commit...${NC}"
git checkout "$OTHER_BRANCH"
git cherry-pick "$COMMIT_HASH"
echo -e "${GREEN}[OK] Cherry-picked onto $OTHER_BRANCH${NC}"

# Push other branch
echo -e "${CYAN}[$OTHER_BRANCH] Pushing...${NC}"
git push origin "$OTHER_BRANCH"
echo -e "${GREEN}[OK] Pushed $OTHER_BRANCH${NC}"

# Return to original branch
git checkout "$ORIGINAL_BRANCH"
echo ""
echo -e "${GREEN}Done! Source changes synced to both '$ORIGINAL_BRANCH' and '$OTHER_BRANCH'.${NC}"
