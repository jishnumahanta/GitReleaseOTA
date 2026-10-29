#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
#  ESP32 OTA GitHub Release Script
#  Bumps version, builds firmware, computes SHA-256, (optionally) signs it,
#  generates version.json, and publishes a GitHub Release.
#
#  Usage:
#    ./release.sh patch "Fix sensor reading"
#    ./release.sh minor "Added weather feature"
#    ./release.sh major "Complete rewrite"
#    ./release.sh patch "Notes" --dry-run      # build + generate, publish nothing, leave files untouched
#    ./release.sh keygen [key.pem]             # create an ECDSA P-256 signing key and print the public key
#
#  Flags:  --dry-run  --allow-dirty  --no-commit
#
#  Signing (recommended): export OTA_SIGNING_KEY=/path/to/ota_signing_key.pem
#  and paste the public key printed by `keygen` into your firmware (otaSetPublicKey).
#
#  Requirements: gh (brew install gh; gh auth login), jq, openssl (signing only),
#  PlatformIO (pio on PATH or at ~/.platformio/penv/bin/pio)
# ─────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── Configuration — edit these for your project (or export them) ──────
GITHUB_USER="${GITHUB_USER:-your-username}"            # GitHub username
GITHUB_REPO="${GITHUB_REPO:-your-releases-repo}"       # Repo that hosts firmware releases (can be separate from source)
VERSION_FILE="${VERSION_FILE:-src/version.h}"          # YOUR firmware file containing: #define FW_VERSION "x.y.z"
BUILD_ENV="${BUILD_ENV:-esp32dev}"                     # PlatformIO environment name (from platformio.ini [env:...])
FIRMWARE_PATH="${FIRMWARE_PATH:-.pio/build/${BUILD_ENV}/firmware.bin}"
OTA_SIGNING_KEY="${OTA_SIGNING_KEY:-}"                 # optional: ECDSA P-256 private key (PEM)
# ─────────────────────────────────────────────────────────────────────

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  $1${NC}"; }
info() { echo -e "${YELLOW}->  $1${NC}"; }
warn() { echo -e "${YELLOW}WARNING: $1${NC}" >&2; }
fail() { echo -e "${RED}ERROR: $1${NC}" >&2; exit 1; }

usage() { sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; }

# ── Arguments ─────────────────────────────────────────────────────────
DRY_RUN=0; ALLOW_DIRTY=0; NO_COMMIT=0
POSITIONAL=()
for arg in "$@"; do
  case "$arg" in
    --dry-run)     DRY_RUN=1 ;;
    --allow-dirty) ALLOW_DIRTY=1 ;;
    --no-commit)   NO_COMMIT=1 ;;
    -h|--help)     usage; exit 0 ;;
    *)             POSITIONAL+=("$arg") ;;
  esac
done
set -- ${POSITIONAL[@]+"${POSITIONAL[@]}"}

# ── keygen subcommand ─────────────────────────────────────────────────
if [ "${1:-}" = "keygen" ]; then
  command -v openssl >/dev/null 2>&1 || fail "openssl not found"
  KEY="${2:-ota_signing_key.pem}"
  [ ! -e "$KEY" ] || fail "$KEY already exists — refusing to overwrite a signing key"
  umask 077
  openssl ecparam -name prime256v1 -genkey -noout -out "$KEY"
  ok "Private key written to $KEY — keep it secret and out of git (it is in .gitignore)."
  echo
  echo "Paste this into your firmware, then call otaSetPublicKey(OTA_PUBLIC_KEY):"
  echo
  echo "const char* OTA_PUBLIC_KEY ="
  openssl ec -in "$KEY" -pubout 2>/dev/null | awk '{ printf "  \"%s\\n\"\n", $0 }' | sed '$ s/$/;/'
  echo
  echo "Then release with:  OTA_SIGNING_KEY=$KEY ./release.sh patch \"notes\""
  exit 0
fi

BUMP="${1:-patch}"
NOTES="${2:-Bug fixes and improvements}"
REPO="${GITHUB_USER}/${GITHUB_REPO}"

# ── 0. Preflight ──────────────────────────────────────────────────────
command -v pio >/dev/null 2>&1 || PATH="$PATH:$HOME/.platformio/penv/bin"
command -v gh  >/dev/null 2>&1 || fail "GitHub CLI not installed. Run: brew install gh"
command -v pio >/dev/null 2>&1 || fail "PlatformIO (pio) not found on PATH or at ~/.platformio/penv/bin/pio"
command -v jq  >/dev/null 2>&1 || fail "jq not installed. Run: brew install jq"
if command -v shasum >/dev/null 2>&1; then SHA_CMD=(shasum -a 256)
elif command -v sha256sum >/dev/null 2>&1; then SHA_CMD=(sha256sum)
else fail "Neither shasum nor sha256sum found"; fi
gh auth status >/dev/null 2>&1 || fail "Not logged in to GitHub. Run: gh auth login"

[ "$GITHUB_USER" != "your-username" ] && [ "$GITHUB_REPO" != "your-releases-repo" ] \
  || fail "Set GITHUB_USER and GITHUB_REPO (edit release.sh or export them)"
case "$BUMP" in major|minor|patch) ;; *) fail "Unknown bump type '$BUMP'. Use: patch | minor | major" ;; esac
[ -f "$VERSION_FILE" ] || fail "VERSION_FILE not found: $VERSION_FILE"

if [ -n "$OTA_SIGNING_KEY" ]; then
  command -v openssl >/dev/null 2>&1 || fail "openssl is required for signing"
  [ -f "$OTA_SIGNING_KEY" ] || fail "OTA_SIGNING_KEY not found: $OTA_SIGNING_KEY"
fi

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  IN_GIT=1
  if [ "$ALLOW_DIRTY" -eq 0 ] && [ -n "$(git status --porcelain)" ]; then
    fail "Working tree has uncommitted changes. Commit or stash them (or pass --allow-dirty)."
  fi
else
  IN_GIT=0
fi

# ── 1. Read current version ───────────────────────────────────────────
# Only a real, uncommented  #define FW_VERSION "x.y.z"  counts (comments and #error text don't).
DEFINE_RE='^[[:space:]]*#[[:space:]]*define[[:space:]]+FW_VERSION[[:space:]]+"[0-9]+\.[0-9]+\.[0-9]+"'
MATCHES=$(grep -cE "$DEFINE_RE" "$VERSION_FILE" || true)
[ "$MATCHES" = "1" ] || fail "Expected exactly one '#define FW_VERSION \"x.y.z\"' in $VERSION_FILE, found $MATCHES"
CURRENT=$(grep -E "$DEFINE_RE" "$VERSION_FILE" | sed -E 's/.*"([0-9]+\.[0-9]+\.[0-9]+)".*/\1/')
info "Current version: $CURRENT"

# ── 2. Bump version ───────────────────────────────────────────────────
IFS='.' read -r MAJ MIN PAT <<< "$CURRENT"
case "$BUMP" in
  major) MAJ=$((MAJ+1)); MIN=0; PAT=0 ;;
  minor) MIN=$((MIN+1)); PAT=0 ;;
  patch) PAT=$((PAT+1)) ;;
esac
NEW_VER="$MAJ.$MIN.$PAT"
TAG="v$NEW_VER"
info "New version:     $NEW_VER"

# ── 3. Don't clobber an existing release; check the repo is reachable ─
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  fail "Release $TAG already exists on $REPO"
fi

WORK=$(mktemp -d)
BACKUP="$WORK/version_file.orig"
cp "$VERSION_FILE" "$BACKUP"
PUBLISHED=0
cleanup() {
  # Anything short of a published release leaves the source tree exactly as we found it.
  if [ "$PUBLISHED" -eq 0 ] && [ -f "$BACKUP" ]; then cat "$BACKUP" > "$VERSION_FILE"; fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# ── 4. Update FW_VERSION ──────────────────────────────────────────────
sed -E "s/^([[:space:]]*#[[:space:]]*define[[:space:]]+FW_VERSION[[:space:]]+\")[0-9]+\.[0-9]+\.[0-9]+(\")/\1${NEW_VER}\2/" \
  "$BACKUP" > "$WORK/version_file.new"
cat "$WORK/version_file.new" > "$VERSION_FILE"
grep -qE "FW_VERSION[[:space:]]+\"${NEW_VER}\"" "$VERSION_FILE" || fail "Failed to write new version into $VERSION_FILE"
ok "FW_VERSION updated to $NEW_VER in $VERSION_FILE"

# ── 5. Build ──────────────────────────────────────────────────────────
info "Building firmware (this takes ~30s)..."
pio run --environment "$BUILD_ENV" || fail "PlatformIO build failed"
[ -f "$FIRMWARE_PATH" ] || fail "firmware.bin not found at $FIRMWARE_PATH"
cp "$FIRMWARE_PATH" "$WORK/firmware.bin"
ok "Build complete"

# ── 6. SHA-256 (+ signature) ──────────────────────────────────────────
SHA=$("${SHA_CMD[@]}" "$WORK/firmware.bin" | awk '{print $1}')
ok "SHA-256: ${SHA:0:16}..."

SIG=""
if [ -n "$OTA_SIGNING_KEY" ]; then
  # Message format must match gro_verify_signature(): "<version>:<sha256 hex>"
  printf '%s' "$NEW_VER:$SHA" | openssl dgst -sha256 -sign "$OTA_SIGNING_KEY" > "$WORK/sig.der"
  SIG=$(openssl base64 -A -in "$WORK/sig.der")
  # Self-check before anything is published
  openssl ec -in "$OTA_SIGNING_KEY" -pubout -out "$WORK/pub.pem" 2>/dev/null
  printf '%s' "$NEW_VER:$SHA" | openssl dgst -sha256 -verify "$WORK/pub.pem" -signature "$WORK/sig.der" >/dev/null \
    || fail "Signature self-check failed"
  ok "Signed with $OTA_SIGNING_KEY"
else
  warn "OTA_SIGNING_KEY not set — release is integrity-checked (SHA-256) but not signed."
fi

# ── 7. Generate version.json (jq handles all JSON escaping) ───────────
jq -n \
  --arg version "$NEW_VER" \
  --arg bin     "https://github.com/$REPO/releases/latest/download/firmware.bin" \
  --arg sha256  "$SHA" \
  --arg sig     "$SIG" \
  --arg notes   "$NOTES" \
  '{version: $version, bin: $bin, sha256: $sha256}
   + (if $sig != "" then {sig: $sig} else {} end)
   + {notes: $notes}' > "$WORK/version.json"
ok "version.json generated"
cat "$WORK/version.json"

if [ "$DRY_RUN" -eq 1 ]; then
  info "--dry-run: nothing published; $VERSION_FILE restored to $CURRENT"
  exit 0
fi

# ── 8. Bootstrap the releases repo only if it is genuinely empty ──────
if ! PROBE=$(gh api "repos/$REPO/commits?per_page=1" 2>&1); then
  if printf '%s' "$PROBE" | grep -qi "empty"; then
    info "Release repo is empty — creating initial commit..."
    README_B64=$(printf '# OTA Release Repository\n\nHosts firmware releases for OTA updates.\n' | base64 | tr -d '\n')
    gh api -X PUT "repos/$REPO/contents/README.md" \
      -f message="chore: init release repo" -f content="$README_B64" >/dev/null \
      || fail "Could not create the initial commit in $REPO"
    ok "Initial commit created"
  else
    # Auth, network, rate-limit or wrong repo name: never guess "empty" and touch the repo.
    fail "Could not read $REPO: $PROBE"
  fi
fi
VIS=$(gh repo view "$REPO" --json visibility -q .visibility 2>/dev/null || true)
if [ -n "$VIS" ] && [ "$VIS" != "PUBLIC" ]; then
  warn "$REPO is $VIS — devices can only download from a public repository."
fi

# ── 9. Publish GitHub Release ─────────────────────────────────────────
info "Publishing release $TAG on $REPO..."
gh release create "$TAG" \
  "$WORK/firmware.bin" \
  "$WORK/version.json" \
  --repo "$REPO" \
  --title "$TAG" \
  --notes "$NOTES" \
  --latest
PUBLISHED=1

ok "Release $TAG published!"
echo ""
echo -e "${GREEN}  $CURRENT -> $NEW_VER released${NC}"
echo -e "${GREEN}  https://github.com/$REPO/releases/tag/$TAG${NC}"
echo ""

# ── 10. Record the bump in the source repo (not pushed) ───────────────
if [ "$IN_GIT" -eq 1 ] && [ "$NO_COMMIT" -eq 0 ]; then
  if git add -- "$VERSION_FILE" && git commit -q -m "release: $TAG" -- "$VERSION_FILE"; then
    ok "Committed version bump ($TAG) locally — push it when ready: git push"
  else
    warn "Release is live but committing the version bump failed; commit $VERSION_FILE yourself."
  fi
fi
