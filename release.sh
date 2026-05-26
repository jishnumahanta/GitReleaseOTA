#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
#  ESP32 OTA GitHub Release Script
#  Bumps version, builds firmware, computes SHA-256,
#  generates version.json, and publishes a GitHub Release.
#
#  Usage:
#    ./release.sh patch "Fix sensor reading"
#    ./release.sh minor "Added weather feature"
#    ./release.sh major "Complete rewrite"
#
#  Requirements:
#    - GitHub CLI:  brew install gh  (then: gh auth login)
#    - PlatformIO:  installed at ~/.platformio/penv/bin/pio
# ─────────────────────────────────────────────────────────────────────

set -e

# ── Configuration — edit these for your project ───────────────────────
GITHUB_USER="your-username"             # GitHub username
GITHUB_REPO="your-releases-repo"       # Repo that hosts firmware releases (can be separate from source)
VERSION_FILE="src/ota_updater.h"       # File containing: #define FW_VERSION "x.y.z"
BUILD_ENV="esp32dev"                   # PlatformIO environment name (from platformio.ini [env:...])
FIRMWARE_PATH=".pio/build/${BUILD_ENV}/firmware.bin"
# ─────────────────────────────────────────────────────────────────────

REPO="${GITHUB_USER}/${GITHUB_REPO}"
BUMP=${1:-patch}
NOTES=${2:-"Bug fixes and improvements"}

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  $1${NC}"; }
info() { echo -e "${YELLOW}->  $1${NC}"; }
fail() { echo -e "${RED}ERROR: $1${NC}"; exit 1; }

# ── 0. Preflight ──────────────────────────────────────────────────────
export PATH="$HOME/.platformio/penv/bin:$PATH"

command -v gh  &>/dev/null || fail "GitHub CLI not installed. Run: brew install gh"
command -v pio &>/dev/null || fail "PlatformIO not found at ~/.platformio/penv/bin/pio"
gh auth status &>/dev/null || fail "Not logged in to GitHub. Run: gh auth login"

[ -f "$VERSION_FILE" ] || fail "VERSION_FILE not found: $VERSION_FILE"

# ── 1. Read current version ───────────────────────────────────────────
CURRENT=$(grep '#define FW_VERSION' "$VERSION_FILE" | sed 's/.*"\(.*\)".*/\1/')
[ -n "$CURRENT" ] || fail "Could not parse FW_VERSION from $VERSION_FILE"
info "Current version: $CURRENT"

# ── 2. Bump version ───────────────────────────────────────────────────
IFS='.' read -r MAJ MIN PAT <<< "$CURRENT"
case "$BUMP" in
  major) MAJ=$((MAJ+1)); MIN=0; PAT=0 ;;
  minor) MIN=$((MIN+1)); PAT=0 ;;
  patch) PAT=$((PAT+1)) ;;
  *)     fail "Unknown bump type '$BUMP'. Use: patch | minor | major" ;;
esac
NEW_VER="$MAJ.$MIN.$PAT"
info "New version:     $NEW_VER"

# ── 3. Update FW_VERSION in header ───────────────────────────────────
sed -i '' "s/#define FW_VERSION \"$CURRENT\"/#define FW_VERSION \"$NEW_VER\"/" "$VERSION_FILE"
ok "FW_VERSION updated to $NEW_VER in $VERSION_FILE"

# ── 4. Build ─────────────────────────────────────────────────────────
info "Building firmware (this takes ~30s)..."
pio run --environment "$BUILD_ENV" || fail "PlatformIO build failed"
[ -f "$FIRMWARE_PATH" ] || fail "firmware.bin not found at $FIRMWARE_PATH"
ok "Build complete"

# ── 5. SHA-256 ───────────────────────────────────────────────────────
SHA=$(shasum -a 256 "$FIRMWARE_PATH" | awk '{print $1}')
ok "SHA-256: ${SHA:0:16}..."

# ── 6. Generate version.json ─────────────────────────────────────────
cat > version.json <<EOF
{
  "version": "$NEW_VER",
  "bin": "https://github.com/$REPO/releases/latest/download/firmware.bin",
  "sha256": "$SHA",
  "notes": "$NOTES"
}
EOF
ok "version.json generated"
cat version.json

# ── 7. Copy binary to project root for upload ─────────────────────────
cp "$FIRMWARE_PATH" ./firmware.bin

# ── 8. Bootstrap repo if empty ────────────────────────────────────────
COMMIT_COUNT=$(gh api "repos/$REPO/commits" --jq 'length' 2>/dev/null || echo "0")
if [ "$COMMIT_COUNT" = "0" ]; then
  info "Release repo is empty — pushing initial commit..."
  TMPDIR_INIT=$(mktemp -d)
  cd "$TMPDIR_INIT"
  git init -q
  git remote add origin "https://github.com/$REPO.git"
  echo "# OTA Release Repository" > README.md
  echo "Hosts firmware releases for OTA updates." >> README.md
  git add README.md
  git -c user.name="OTA Release" -c user.email="ota@device.local" \
    commit -q -m "chore: init release repo"
  git push -q --force origin HEAD:main
  cd - > /dev/null
  rm -rf "$TMPDIR_INIT"
  ok "Initial commit pushed"
fi

# ── 9. Publish GitHub Release ─────────────────────────────────────────
TAG="v$NEW_VER"
info "Publishing release $TAG on $REPO..."

gh release create "$TAG" \
  firmware.bin \
  version.json \
  --repo "$REPO" \
  --title "$TAG" \
  --notes "$NOTES" \
  --latest

ok "Release $TAG published!"
echo ""
echo -e "${GREEN}  $CURRENT -> $NEW_VER released${NC}"
echo -e "${GREEN}  https://github.com/$REPO/releases/tag/$TAG${NC}"
echo ""

# ── 10. Clean up ──────────────────────────────────────────────────────
rm -f ./firmware.bin
