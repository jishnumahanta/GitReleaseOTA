#pragma once
// ─────────────────────────────────────────────────────────────────────
//  GitReleaseOTA core helpers — pure C++, no Arduino/ESP32 dependency,
//  so they can be unit-tested on a host machine (see tests/).
// ─────────────────────────────────────────────────────────────────────
#include <stddef.h>
#include <stdint.h>
#include <mbedtls/md.h>

// Semantic-version compare. Accepts "1.2.3", "v1.2", "1.2.3-rc.1", "1.2.3+build".
// Pre-releases sort below the release (1.0.0-rc1 < 1.0.0); build metadata is ignored.
// Returns <0, 0, >0. Sets *ok=false (and returns 0) if either string is not a version.
int gro_version_compare(const char *a, const char *b, bool *ok);

// Extract a top-level string value from a flat JSON object.
// Handles whitespace, escapes (\" \\ \n \uXXXX ...), and ignores keys that only appear
// inside other values or nested objects. Returns false if the key is missing, the
// value is not a string, or the JSON is malformed. Output is always NUL-terminated
// (and truncated to fit `cap`).
bool gro_json_get_string(const char *json, const char *key, char *out, size_t cap);

// True if `s` is exactly 64 hex characters.
bool gro_is_sha256_hex(const char *s);

// Streaming SHA-256 (hardware accelerated on ESP32 through mbedTLS).
class GroSha256 {
public:
  GroSha256();
  ~GroSha256();
  GroSha256(const GroSha256 &) = delete;
  GroSha256 &operator=(const GroSha256 &) = delete;

  bool begin();
  void update(const uint8_t *data, size_t len);
  bool finishHex(char out[65]);   // lowercase hex + NUL

private:
  mbedtls_md_context_t _ctx;
  bool _started;
};

// Verify an ECDSA (P-256 / SHA-256) signature over the ASCII message
// "<version>:<sha256 lowercase hex>". `pubKeyPem` is a NUL-terminated PEM public key,
// `sigB64` the base64 DER signature (what `openssl dgst -sha256 -sign key.pem | base64`
// produces). Returns true only when the signature is valid for that key.
bool gro_verify_signature(const char *pubKeyPem, const char *version,
                          const char *sha256hex, const char *sigB64);
