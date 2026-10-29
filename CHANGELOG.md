# Changelog

## 1.1.0

### Fixed
- **Examples (and any Arduino IDE project that set `FW_VERSION` / `OTA_GITHUB_*` in the sketch) failed to compile** — the library is built as a separate translation unit that never saw the sketch's `#define`s and hit the header's `#error`. Configuration is now a runtime call, `otaBegin(user, repo, version)`; the old macros still work through `otaInit()`.
- `release.sh` could **force-push over the releases repo** whenever the "is it empty?" API call failed for any reason (network, auth, rate limit). It now only bootstraps when GitHub reports an empty repo, does it through the contents API, and never force-pushes.
- `release.sh` default `VERSION_FILE` pointed at the library header and matched `#define FW_VERSION` inside comments and the `#error` text. It now requires exactly one real, uncommented `#define FW_VERSION "x.y.z"`.
- Release notes containing quotes, backslashes or newlines produced invalid `version.json`. JSON is now built with `jq`.
- `release.sh` used macOS-only `sed -i ''`, left the bumped version behind when a build failed, did not check for a dirty tree or an existing release, and never recorded the bump. It is now portable, restores the version file on any failure before publishing, refuses dirty trees / existing tags, and commits the bump locally (not pushed).
- Data race on the shared OTA state between the check task and the caller (could corrupt `String`s); `s_busy` was a plain bool and a check and an install (or two installs) could run at once. State is now mutex-protected and concurrent operations are refused.
- A truncated download (connection dropped before `Content-Length`) was not detected until `Update.end()`; it is now an explicit error.
- `otaInstall()` could install without a prior check (and without a hash). It now requires a successful check that found an update.
- JSON extraction mis-parsed escaped quotes, non-string values, whitespace variants and keys that appear inside other values; replaced with a small, tested parser.
- `v1.2.3`, `1.2.3-rc1` and other non-`x.y.z` strings were silently compared as `0.0.0`; versions are now parsed as semver and invalid ones are `OTA_ERROR_PARSE`.
- `FreeRTOSMultiTask` ran `otaInstall()` from a 4 KB task; the TLS handshake needs ~10 KB. Now 12 KB.
- `library.properties` `includes=` listed core libraries instead of the library's own header.
- Failure paths now share one cleanup routine, so `Update.abort()`, `http.end()` and clearing `g_otaInProgress` can't be skipped.
- Progress logging printed irregularly (`written % 64K` test); it now reports every 64 KB.
- Removed an unused SHA-256 helper and duplicated hashing code.

### Added
- Optional **ECDSA P-256 signatures**: `otaSetPublicKey()`, `OTA_SIGNING_KEY` and `./release.sh keygen`. States `OTA_ERROR_SIGNATURE` and `OTA_ERROR_CONFIG`.
- `otaSetFailureCallback()` (restart hardware after a failed install), `otaSetProgressCallback()`, `otaSetCACert()` (optional TLS validation), `otaAllowUnverified()`.
- `otaIsPendingVerify()` / `otaMarkValid()` wrappers for ESP-IDF app rollback (automatic rollback needs a bootloader built with `CONFIG_APP_ROLLBACK_ENABLE`).
- `GitReleaseOTA.h` (Arduino-style header; `ota_updater.h` still works), `keywords.txt`.
- `release.sh --dry-run`, `--allow-dirty`, `--no-commit`.
- Host unit tests, `release.sh` end-to-end tests and a CI workflow.

### Changed (behaviour)
- A `version.json` without `sha256` is **refused** unless `otaAllowUnverified(true)`. `release.sh` always writes it.
- `bin` in `version.json` must be an `https://` URL.
- `version.json` larger than 4 KB is rejected.
- `OTA_VERSION_URL` / `OTA_BIN_URL` macros were removed (they were internal).
- `OtaInfo` gained a trailing `signatureVerified` field; `OtaState` gained two values at the end.
- The README's security section no longer claims SHA-256 makes unauthenticated TLS acceptable — it doesn't; signatures are the recommended trust mechanism.

## 1.0.2
- Initial public releases (PlatformIO Registry / Arduino Library Manager metadata fixes).
