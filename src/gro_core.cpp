#include "gro_core.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>

// ── Version compare ───────────────────────────────────────────────────

namespace {

struct SemVer {
  unsigned long major = 0, minor = 0, patch = 0;
  char pre[40] = {0};
  bool ok = false;
};

SemVer parseVersion(const char *s) {
  SemVer v;
  if (!s) return v;
  if (*s == 'v' || *s == 'V') s++;
  unsigned long *parts[3] = {&v.major, &v.minor, &v.patch};
  for (int i = 0; i < 3; i++) {
    if (!isdigit((unsigned char)*s)) return v;
    char *end = nullptr;
    *parts[i] = strtoul(s, &end, 10);
    s = end;
    if (*s != '.') break;
    s++;                                   // "1." with nothing after is rejected next loop / below
    if (i == 2) return v;                  // a fourth numeric component is not semver
    if (!isdigit((unsigned char)*s)) return v;
  }
  if (*s == '-') {
    s++;
    size_t n = 0;
    while (*s && *s != '+') {
      if (n + 1 >= sizeof(v.pre)) return v;   // absurdly long pre-release tag
      v.pre[n++] = *s++;
    }
    if (n == 0) return v;
  }
  if (*s == '+') {
    while (*s) s++;                        // build metadata ignored
  }
  if (*s != '\0') return v;
  v.ok = true;
  return v;
}

bool allDigits(const char *s, size_t n) {
  if (n == 0) return false;
  for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return false;
  return true;
}

// SemVer §11: compare dot-separated pre-release identifiers.
int comparePre(const char *a, const char *b) {
  while (*a || *b) {
    const char *ae = strchr(a, '.');
    const char *be = strchr(b, '.');
    size_t al = ae ? (size_t)(ae - a) : strlen(a);
    size_t bl = be ? (size_t)(be - b) : strlen(b);
    if (al == 0 && !*a) return -1;         // a ran out: fewer fields sorts lower
    if (bl == 0 && !*b) return 1;
    bool an = allDigits(a, al), bn = allDigits(b, bl);
    int c;
    if (an && bn) {
      unsigned long x = strtoul(a, nullptr, 10), y = strtoul(b, nullptr, 10);
      c = (x < y) ? -1 : (x > y) ? 1 : 0;
    } else if (an != bn) {
      c = an ? -1 : 1;                      // numeric identifiers sort below alphanumeric
    } else {
      size_t m = al < bl ? al : bl;
      c = strncmp(a, b, m);
      if (c == 0) c = (al < bl) ? -1 : (al > bl) ? 1 : 0;
    }
    if (c) return c < 0 ? -1 : 1;
    a = ae ? ae + 1 : a + al;
    b = be ? be + 1 : b + bl;
  }
  return 0;
}

}  // namespace

int gro_version_compare(const char *a, const char *b, bool *ok) {
  SemVer x = parseVersion(a), y = parseVersion(b);
  if (!x.ok || !y.ok) {
    if (ok) *ok = false;
    return 0;
  }
  if (ok) *ok = true;
  if (x.major != y.major) return x.major < y.major ? -1 : 1;
  if (x.minor != y.minor) return x.minor < y.minor ? -1 : 1;
  if (x.patch != y.patch) return x.patch < y.patch ? -1 : 1;
  bool xp = x.pre[0], yp = y.pre[0];
  if (!xp && !yp) return 0;
  if (!xp) return 1;
  if (!yp) return -1;
  return comparePre(x.pre, y.pre);
}

// ── JSON string extraction ────────────────────────────────────────────

namespace {

void putUtf8(unsigned cp, char *out, size_t cap, size_t &n) {
  char tmp[4];
  size_t k = 0;
  if (cp < 0x80) {
    tmp[k++] = (char)cp;
  } else if (cp < 0x800) {
    tmp[k++] = (char)(0xC0 | (cp >> 6));
    tmp[k++] = (char)(0x80 | (cp & 0x3F));
  } else {
    tmp[k++] = (char)(0xE0 | (cp >> 12));
    tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    tmp[k++] = (char)(0x80 | (cp & 0x3F));
  }
  for (size_t i = 0; i < k; i++) if (out && n + 1 < cap) out[n++] = tmp[i];
}

// `p` points at the opening quote. Returns the pointer just past the closing quote,
// or nullptr if the string is malformed. `out` may be null (skip only).
const char *parseString(const char *p, char *out, size_t cap) {
  size_t n = 0;
  p++;
  while (*p && *p != '"') {
    if (*p != '\\') {
      if (out && n + 1 < cap) out[n++] = *p;
      p++;
      continue;
    }
    p++;
    switch (*p) {
      case '"':  if (out && n + 1 < cap) out[n++] = '"';  break;
      case '\\': if (out && n + 1 < cap) out[n++] = '\\'; break;
      case '/':  if (out && n + 1 < cap) out[n++] = '/';  break;
      case 'n':  if (out && n + 1 < cap) out[n++] = '\n'; break;
      case 't':  if (out && n + 1 < cap) out[n++] = '\t'; break;
      case 'r':  if (out && n + 1 < cap) out[n++] = '\r'; break;
      case 'b':  if (out && n + 1 < cap) out[n++] = '\b'; break;
      case 'f':  if (out && n + 1 < cap) out[n++] = '\f'; break;
      case 'u': {
        unsigned cp = 0;
        for (int i = 1; i <= 4; i++) {
          if (!isxdigit((unsigned char)p[i])) return nullptr;
          char c = p[i];
          cp = cp * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10));
        }
        p += 4;
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';   // surrogate halves: not worth decoding here
        putUtf8(cp, out, cap, n);
        break;
      }
      default:
        return nullptr;
    }
    p++;
  }
  if (*p != '"') return nullptr;
  if (out && cap) out[n] = '\0';
  return p + 1;
}

const char *skipWs(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}

}  // namespace

bool gro_json_get_string(const char *json, const char *key, char *out, size_t cap) {
  if (!json || !key || !out || cap == 0) return false;
  out[0] = '\0';
  const size_t keyLen = strlen(key);
  const char *p = json;
  int depth = 0;
  while (*p) {
    char c = *p;
    if (c == '{' || c == '[') { depth++; p++; continue; }
    if (c == '}' || c == ']') { depth--; p++; continue; }
    if (c != '"') { p++; continue; }

    // A string: either an object key (followed by ':') or a value.
    char scratch[48];
    const bool atTop = (depth == 1);
    const char *next = parseString(p, atTop ? scratch : nullptr, sizeof(scratch));
    if (!next) return false;
    const char *after = skipWs(next);
    if (atTop && *after == ':' && strlen(scratch) == keyLen && strcmp(scratch, key) == 0 &&
        keyLen < sizeof(scratch) - 1) {
      after = skipWs(after + 1);
      if (*after != '"') return false;
      return parseString(after, out, cap) != nullptr;
    }
    p = next;
  }
  return false;
}

bool gro_is_sha256_hex(const char *s) {
  if (!s || strlen(s) != 64) return false;
  for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

// ── SHA-256 ───────────────────────────────────────────────────────────

static void toHex(const uint8_t h[32], char out[65]) {
  static const char *digits = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[2 * i]     = digits[h[i] >> 4];
    out[2 * i + 1] = digits[h[i] & 0x0F];
  }
  out[64] = '\0';
}

GroSha256::GroSha256() : _started(false) { mbedtls_md_init(&_ctx); }

GroSha256::~GroSha256() { mbedtls_md_free(&_ctx); }

bool GroSha256::begin() {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  if (mbedtls_md_setup(&_ctx, info, 0) != 0) return false;
  if (mbedtls_md_starts(&_ctx) != 0) return false;
  _started = true;
  return true;
}

void GroSha256::update(const uint8_t *data, size_t len) {
  if (_started) mbedtls_md_update(&_ctx, data, len);
}

bool GroSha256::finishHex(char out[65]) {
  if (!_started) return false;
  uint8_t hash[32];
  if (mbedtls_md_finish(&_ctx, hash) != 0) return false;
  _started = false;
  toHex(hash, out);
  return true;
}

// ── Signature verification ────────────────────────────────────────────

bool gro_verify_signature(const char *pubKeyPem, const char *version,
                          const char *sha256hex, const char *sigB64) {
  if (!pubKeyPem || !version || !sha256hex || !sigB64) return false;
  if (!gro_is_sha256_hex(sha256hex)) return false;

  char msg[128];
  char shaLower[65];
  for (int i = 0; i < 64; i++) shaLower[i] = (char)tolower((unsigned char)sha256hex[i]);
  shaLower[64] = '\0';
  int n = snprintf(msg, sizeof(msg), "%s:%s", version, shaLower);
  if (n < 0 || (size_t)n >= sizeof(msg)) return false;

  uint8_t hash[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md(info, (const uint8_t *)msg, (size_t)n, hash) != 0) return false;

  uint8_t sig[160];
  size_t sigLen = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen, (const uint8_t *)sigB64, strlen(sigB64)) != 0 ||
      sigLen == 0) {
    return false;
  }

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  bool valid = false;
  // PEM input must include the terminating NUL in the length.
  // A 256-bit key means P-256 (what release.sh keygen makes); this API exists in every mbedTLS release.
  if (mbedtls_pk_parse_public_key(&pk, (const uint8_t *)pubKeyPem, strlen(pubKeyPem) + 1) == 0 &&
      mbedtls_pk_get_bitlen(&pk) == 256) {
    valid = (mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sigLen) == 0);
  }
  mbedtls_pk_free(&pk);
  return valid;
}
