// Host-side unit tests for src/gro_core.cpp. Run with tests/run_host_tests.sh.
#include "../../src/gro_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <sstream>
#include <string>
#if __has_include(<psa/crypto.h>)
#include <psa/crypto.h>
#endif

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int cmp(const char *a, const char *b, bool *ok = nullptr) {
  bool o = false;
  int r = gro_version_compare(a, b, &o);
  if (ok) *ok = o;
  return r;
}
static bool bad(const char *a) { bool ok = true; gro_version_compare(a, "1.0.0", &ok); return !ok; }

static void testVersions() {
  CHECK(cmp("1.10.0", "1.9.0") > 0);          // not lexicographic
  CHECK(cmp("2.0.0", "1.99.99") > 0);
  CHECK(cmp("1.0.0", "1.0.0") == 0);
  CHECK(cmp("1.0.3", "1.0.2") > 0);
  CHECK(cmp("1.0.2", "1.0.3") < 0);
  CHECK(cmp("v1.2.3", "1.2.3") == 0);         // leading v
  CHECK(cmp("1.2", "1.2.0") == 0);            // missing patch
  CHECK(cmp("1.0.0-rc1", "1.0.0") < 0);       // pre-release < release
  CHECK(cmp("1.0.0", "1.0.0-rc1") > 0);
  CHECK(cmp("1.0.1-rc1", "1.0.0") > 0);
  CHECK(cmp("1.0.0-rc.1", "1.0.0-rc.2") < 0);
  CHECK(cmp("1.0.0-alpha", "1.0.0-alpha.1") < 0);        // semver.org §11 examples
  CHECK(cmp("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0);
  CHECK(cmp("1.0.0-alpha.beta", "1.0.0-beta") < 0);
  CHECK(cmp("1.0.0-beta.2", "1.0.0-beta.11") < 0);
  CHECK(cmp("1.0.0-beta.11", "1.0.0-rc.1") < 0);
  CHECK(cmp("1.0.0+build5", "1.0.0") == 0);   // build metadata ignored
  CHECK(cmp("1.0.0-rc1+x", "1.0.0-rc1") == 0);
  const char *invalid[] = {"", "abc", "1..2", "1.2.3.4", "1.2.3-", "1.2.x", ".1", "1.", "v", "1.2.3 ", "-1.0.0"};
  for (const char *s : invalid) CHECK(bad(s));
  bool ok = true;
  gro_version_compare(nullptr, "1.0.0", &ok);
  CHECK(!ok);
}

static std::string get(const char *json, const char *key, bool *found = nullptr) {
  char out[64];
  bool f = gro_json_get_string(json, key, out, sizeof(out));
  if (found) *found = f;
  return f ? out : "";
}

static void testJson() {
  const char *plain = "{\n  \"version\": \"1.0.3\",\n  \"bin\": \"https://x/y.bin\",\n  \"sha256\": \"abc\",\n  \"notes\": \"Fix crash\"\n}";
  CHECK(get(plain, "version") == "1.0.3");
  CHECK(get(plain, "bin") == "https://x/y.bin");
  CHECK(get(plain, "notes") == "Fix crash");
  bool f = true;
  get(plain, "missing", &f);
  CHECK(!f);

  // compact, odd whitespace
  CHECK(get("{\"version\":\"2.0.0\"}", "version") == "2.0.0");
  CHECK(get("{ \"version\" \t:\r\n \"2.0.0\" }", "version") == "2.0.0");

  // escapes in values (this is what an unescaped-quote release note used to break)
  CHECK(get("{\"notes\": \"Fixed \\\"sensor\\\" bug\\nline2\"}", "notes") == "Fixed \"sensor\" bug\nline2");
  CHECK(get("{\"notes\": \"back\\\\slash \\/ tab\\t\"}", "notes") == "back\\slash / tab\t");
  CHECK(get("{\"notes\": \"caf\\u00e9 \\u20ac\"}", "notes") == "caf\xC3\xA9 \xE2\x82\xAC");

  // a key name that only appears inside another value must not match
  CHECK(get("{\"notes\": \"bumped \\\"version\\\": \\\"9.9.9\\\"\", \"version\": \"1.0.0\"}", "version") == "1.0.0");
  CHECK(get("{\"notes\": \"the version\", \"version\": \"1.0.0\"}", "version") == "1.0.0");
  // ...nor keys inside nested objects/arrays
  CHECK(get("{\"meta\": {\"version\": \"7.7.7\"}, \"version\": \"1.0.0\"}", "version") == "1.0.0");
  get("{\"meta\": {\"version\": \"7.7.7\"}}", "version", &f);
  CHECK(!f);
  get("{\"list\": [\"version\", \"x\"]}", "version", &f);
  CHECK(!f);

  // non-string value / malformed input
  get("{\"version\": 1.0}", "version", &f);
  CHECK(!f);
  get("{\"version\": \"1.0", "version", &f);
  CHECK(!f);
  get("{\"version\": \"bad\\q\"}", "version", &f);
  CHECK(!f);
  get("", "version", &f);
  CHECK(!f);
  get("not json", "version", &f);
  CHECK(!f);

  // truncation is safe and NUL-terminated
  char small[5];
  CHECK(gro_json_get_string("{\"k\": \"abcdefghij\"}", "k", small, sizeof(small)));
  CHECK(strcmp(small, "abcd") == 0);
  char one[1];
  gro_json_get_string("{\"k\": \"abc\"}", "k", one, sizeof(one));
  CHECK(one[0] == '\0');
}

static void testSha() {
  GroSha256 s;
  CHECK(s.begin());
  s.update((const uint8_t *)"a", 1);
  s.update((const uint8_t *)"bc", 2);
  char hex[65];
  CHECK(s.finishHex(hex));
  CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

  GroSha256 unstarted;
  CHECK(!unstarted.finishHex(hex));

  CHECK(gro_is_sha256_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK(gro_is_sha256_hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
  CHECK(!gro_is_sha256_hex("ba78"));
  CHECK(!gro_is_sha256_hex("zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK(!gro_is_sha256_hex(nullptr));
}

static std::string slurp(const char *path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static void testSignature() {
  const char *pubPath = getenv("T_PUB"), *otherPath = getenv("T_PUB_OTHER"), *sigPath = getenv("T_SIG");
  const char *ver = getenv("T_VER"), *sha = getenv("T_SHA");
  if (!pubPath || !otherPath || !sigPath || !ver || !sha) {
    printf("SKIP signature tests (T_PUB/T_PUB_OTHER/T_SIG/T_VER/T_SHA not set)\n");
    return;
  }
  std::string pub = slurp(pubPath), other = slurp(otherPath), sig = slurp(sigPath);
  while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r')) sig.pop_back();
  std::string shaUpper = sha;
  for (auto &c : shaUpper) c = (char)toupper((unsigned char)c);

  CHECK(gro_verify_signature(pub.c_str(), ver, sha, sig.c_str()));            // produced by `openssl dgst -sign`
  CHECK(gro_verify_signature(pub.c_str(), ver, shaUpper.c_str(), sig.c_str())); // hash case is normalised
  CHECK(!gro_verify_signature(pub.c_str(), "9.9.9", sha, sig.c_str()));       // version is covered by the signature
  std::string tampered = sha;
  tampered[0] = (tampered[0] == '0') ? '1' : '0';
  CHECK(!gro_verify_signature(pub.c_str(), ver, tampered.c_str(), sig.c_str())); // hash is covered
  CHECK(!gro_verify_signature(other.c_str(), ver, sha, sig.c_str()));         // wrong key
  CHECK(!gro_verify_signature(pub.c_str(), ver, sha, "AAAA"));                // garbage signature
  CHECK(!gro_verify_signature(pub.c_str(), ver, sha, "!!!not base64!!!"));
  CHECK(!gro_verify_signature(pub.c_str(), ver, sha, ""));
  CHECK(!gro_verify_signature("not a pem", ver, sha, sig.c_str()));
  CHECK(!gro_verify_signature(pub.c_str(), ver, "tooshort", sig.c_str()));
  CHECK(!gro_verify_signature(nullptr, ver, sha, sig.c_str()));
  std::string flipped = sig;
  flipped[10] = (flipped[10] == 'A') ? 'B' : 'A';
  CHECK(!gro_verify_signature(pub.c_str(), ver, sha, flipped.c_str()));       // one corrupted signature byte
}

int main() {
#if defined(PSA_CRYPTO_H) || __has_include(<psa/crypto.h>)
  psa_crypto_init();   // required by mbedTLS 4.x; harmless on 3.x
#endif
  testVersions();
  testJson();
  testSha();
  testSignature();
  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
