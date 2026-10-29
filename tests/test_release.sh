#!/usr/bin/env bash
# End-to-end tests for release.sh using stub `pio` and `gh` executables (nothing touches GitHub).
# Needs: bash, git, jq, openssl, shasum|sha256sum.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE="$(dirname "$HERE")/release.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

PASS=0; FAIL=0
check() { # check "description" command...
  local desc="$1"; shift
  if "$@" >/dev/null 2>&1; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FAIL: $desc"; fi
}

# ── stubs ─────────────────────────────────────────────────────────────
mkdir -p "$T/bin"
cat > "$T/bin/pio" <<'EOF'
#!/usr/bin/env bash
[ "${STUB_PIO_FAIL:-0}" = "1" ] && exit 1
mkdir -p .pio/build/esp32dev
printf 'fake-firmware-%s' "$(grep -o 'FW_VERSION "[^"]*"' src/version.h | head -1)" > .pio/build/esp32dev/firmware.bin
EOF
cat > "$T/bin/gh" <<'EOF'
#!/usr/bin/env bash
echo "gh $*" >> "$STUB_LOG"
case "$1" in
  auth) exit 0 ;;
  repo) echo PUBLIC; exit 0 ;;
  release)
    case "$2" in
      view)   [ "${STUB_RELEASE_EXISTS:-0}" = "1" ] && exit 0 || exit 1 ;;
      create) # copy the uploaded assets where the test can look at them
              shift 2; mkdir -p "$STUB_ASSETS"
              for a in "$@"; do case "$a" in /*) cp "$a" "$STUB_ASSETS/";; esac; done
              exit 0 ;;
    esac ;;
  api)
    for a in "$@"; do [ "$a" = "PUT" ] && exit 0; done
    case "${STUB_COMMITS:-ok}" in
      ok)    echo '[{"sha":"abc"}]'; exit 0 ;;
      empty) echo '{"message":"Git Repository is empty.","status":"409"}'; exit 1 ;;
      error) echo 'error connecting to api.github.com'; exit 1 ;;
    esac ;;
esac
exit 0
EOF
chmod +x "$T/bin/pio" "$T/bin/gh"
export PATH="$T/bin:$PATH"
export STUB_LOG="$T/gh.log" STUB_ASSETS="$T/assets"

# ── a throwaway firmware repo ─────────────────────────────────────────
new_project() {
  P="$T/proj-$1"; mkdir -p "$P/src"; cd "$P" || exit 1
  git init -q; git config user.name test; git config user.email test@example.com
  cat > src/version.h <<'EOF'
// Docs: set it like this:
//   #define FW_VERSION       "9.9.9"
#pragma once
#define FW_VERSION "1.2.3"
EOF
  printf '.pio/\n' > .gitignore
  git add -A; git commit -q -m init
  : > "$STUB_LOG"; rm -rf "$STUB_ASSETS"
  export GITHUB_USER=alice GITHUB_REPO=fw-releases
  unset OTA_SIGNING_KEY STUB_PIO_FAIL STUB_RELEASE_EXISTS STUB_COMMITS
}
version_in_file() { grep -E '^#define FW_VERSION' src/version.h | sed -E 's/.*"(.*)".*/\1/'; }
log_has() { grep -q -- "$1" "$STUB_LOG"; }

# 1. happy path: bump only the real define (not comments), JSON valid with hostile notes, hash matches
new_project happy
NOTES=$'Fixed "sensor" bug\nsecond line \\ backslash'
"$RELEASE" patch "$NOTES" >"$T/out1" 2>&1; RC=$?
check "happy path exits 0"                    test $RC -eq 0
check "version bumped to 1.2.4"               test "$(version_in_file)" = "1.2.4"
check "comment line untouched"                grep -q '//   #define FW_VERSION       "9.9.9"' src/version.h
check "version.json is valid JSON"            jq -e . "$STUB_ASSETS/version.json"
check "notes survive round-trip"              test "$(jq -r .notes "$STUB_ASSETS/version.json")" = "$NOTES"
check "version.json version"                  test "$(jq -r .version "$STUB_ASSETS/version.json")" = "1.2.4"
EXPECT_SHA=$( (shasum -a 256 "$STUB_ASSETS/firmware.bin" 2>/dev/null || sha256sum "$STUB_ASSETS/firmware.bin") | awk '{print $1}')
check "sha256 matches the uploaded binary"    test "$(jq -r .sha256 "$STUB_ASSETS/version.json")" = "$EXPECT_SHA"
check "no sig when no key"                    test "$(jq 'has("sig")' "$STUB_ASSETS/version.json")" = "false"
check "release tagged v1.2.4"                 log_has "release create v1.2.4"
check "bump committed locally"                test "$(git log -1 --format=%s)" = "release: v1.2.4"
check "tree clean afterwards"                 test -z "$(git status --porcelain)"
# commit is authored by the repo's configured user, with no extra trailers
check "no co-author trailer"                  bash -c '! git log -1 --format=%B | grep -qi "co-authored-by"'

# 2. minor / major bump math
new_project minor; "$RELEASE" minor "n" >/dev/null 2>&1; check "minor bump 1.3.0" test "$(version_in_file)" = "1.3.0"
new_project major; "$RELEASE" major "n" >/dev/null 2>&1; check "major bump 2.0.0" test "$(version_in_file)" = "2.0.0"

# 3. signing: signature verifies against the public key for "<version>:<sha>"
new_project signed
openssl ecparam -name prime256v1 -genkey -noout -out "$T/key.pem" 2>/dev/null
openssl ec -in "$T/key.pem" -pubout -out "$T/pub.pem" 2>/dev/null
OTA_SIGNING_KEY="$T/key.pem" "$RELEASE" patch "signed" >/dev/null 2>&1
check "sig present"                           test "$(jq 'has("sig")' "$STUB_ASSETS/version.json")" = "true"
jq -r .sig "$STUB_ASSETS/version.json" | openssl base64 -d -A > "$T/sig.der"
MSG="$(jq -r .version "$STUB_ASSETS/version.json"):$(jq -r .sha256 "$STUB_ASSETS/version.json")"
check "signature verifies"                    bash -c "printf '%s' '$MSG' | openssl dgst -sha256 -verify '$T/pub.pem' -signature '$T/sig.der'"
check "wrong message fails verification"      bash -c "! printf '%s' 'x$MSG' | openssl dgst -sha256 -verify '$T/pub.pem' -signature '$T/sig.der'"

# 4. THE DANGEROUS CASE: API error must never be treated as "empty repo"
new_project apierr
STUB_COMMITS=error "$RELEASE" patch "n" >"$T/out4" 2>&1; RC=$?
check "api error: script fails"               test $RC -ne 0
check "api error: no repo writes"             bash -c '! grep -q "PUT" "'"$STUB_LOG"'"'
check "api error: no release created"         bash -c '! grep -q "release create" "'"$STUB_LOG"'"'
check "api error: version file restored"      test "$(version_in_file)" = "1.2.3"
check "api error: tree clean"                 test -z "$(git status --porcelain)"

# 5. genuinely empty repo is bootstrapped via the contents API (no force-push anywhere)
new_project empty
STUB_COMMITS=empty "$RELEASE" patch "n" >/dev/null 2>&1; RC=$?
check "empty repo: succeeds"                  test $RC -eq 0
check "empty repo: README created via API"    log_has "contents/README.md"
check "empty repo: release created"           log_has "release create v1.2.4"
check "empty repo: no git push --force"       bash -c '! grep -qi "force" "'"$STUB_LOG"'"'

# 6. failure modes leave the tree untouched
new_project build; STUB_PIO_FAIL=1 "$RELEASE" patch "n" >/dev/null 2>&1; RC=$?
check "build failure: exits non-zero"         test $RC -ne 0
check "build failure: version restored"       test "$(version_in_file)" = "1.2.3"
check "build failure: nothing published"      bash -c '! grep -q "release create" "'"$STUB_LOG"'"'

new_project dirty; echo "// wip" >> src/version.h
"$RELEASE" patch "n" >/dev/null 2>&1; RC=$?
check "dirty tree: refused"                   test $RC -ne 0
check "dirty tree: nothing published"         bash -c '! grep -q "release create" "'"$STUB_LOG"'"'

new_project exists; STUB_RELEASE_EXISTS=1 "$RELEASE" patch "n" >/dev/null 2>&1; RC=$?
check "existing release: refused"             test $RC -ne 0
check "existing release: version untouched"   test "$(version_in_file)" = "1.2.3"

new_project dry; "$RELEASE" patch "n" --dry-run >/dev/null 2>&1; RC=$?
check "dry-run: exits 0"                      test $RC -eq 0
check "dry-run: nothing published"            bash -c '! grep -q "release create" "'"$STUB_LOG"'"'
check "dry-run: version restored"             test "$(version_in_file)" = "1.2.3"
check "dry-run: tree clean"                   test -z "$(git status --porcelain)"

new_project placeholder; GITHUB_USER=your-username "$RELEASE" patch "n" >/dev/null 2>&1; RC=$?
check "placeholder config: refused"           test $RC -ne 0

new_project badversion; printf '#define FW_VERSION "1.2"\n' > src/version.h; git commit -qam v
"$RELEASE" patch "n" >/dev/null 2>&1; RC=$?
check "non-x.y.z version: refused"            test $RC -ne 0

new_project nodefine; printf '// #define FW_VERSION "1.2.3"\n' > src/version.h; git commit -qam v
"$RELEASE" patch "n" >/dev/null 2>&1; RC=$?
check "commented-out define: refused"         test $RC -ne 0

new_project badbump; "$RELEASE" huge "n" >/dev/null 2>&1; RC=$?
check "unknown bump type: refused"            test $RC -ne 0

# 7. keygen
cd "$T" || exit 1
"$RELEASE" keygen "$T/gen.pem" >"$T/keygen.out" 2>&1; RC=$?
check "keygen exits 0"                        test $RC -eq 0
check "keygen writes private key"             test -s "$T/gen.pem"
check "keygen prints C string"                grep -q 'BEGIN PUBLIC KEY' "$T/keygen.out"
"$RELEASE" keygen "$T/gen.pem" >/dev/null 2>&1; RC=$?
check "keygen refuses to overwrite a key"     test $RC -ne 0

echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
