#include "omada_device.h"

#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <esp_random.h>
#include <esp_log.h>
#include "logger.h"   // must be included LAST — see logger.h

static const char* TAG = "omadaDev";

// ── hex helper ────────────────────────────────────────────────────────────────
static String toHexUpper(const uint8_t* b, size_t n) {
  static const char* H = "0123456789ABCDEF";
  String s; s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += H[b[i] >> 4]; s += H[b[i] & 0xF]; }
  return s;
}

// ── MD5 / SHA256 via the generic mbedtls_md interface (already linked) ─────────
static void mdOneShot(mbedtls_md_type_t type, const uint8_t* data, size_t len, uint8_t* out) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type);
  mbedtls_md(info, data, len, out);
}

String ecspMd5Upper(const uint8_t* data, size_t len) {
  uint8_t d[16]; mdOneShot(MBEDTLS_MD_MD5, data, len, d); return toHexUpper(d, 16);
}
String ecspSha256Upper(const uint8_t* data, size_t len) {
  uint8_t d[32]; mdOneShot(MBEDTLS_MD_SHA256, data, len, d); return toHexUpper(d, 32);
}

// ── ecsp2_auth ────────────────────────────────────────────────────────────────
String ecsp2AuthMd5(const String& username, const String& md5UpperHex, const String& randomKey) {
  String innerIn = username + md5UpperHex;
  String inner   = ecspSha256Upper((const uint8_t*)innerIn.c_str(), innerIn.length());
  String outerIn = inner + randomKey;
  return ecspSha256Upper((const uint8_t*)outerIn.c_str(), outerIn.length());
}
String ecsp2Auth(const String& username, const String& password, const String& randomKey) {
  String md5pw = ecspMd5Upper((const uint8_t*)password.c_str(), password.length());
  return ecsp2AuthMd5(username, md5pw, randomKey);
}

// ── RC4 (standard) ────────────────────────────────────────────────────────────
void ecspRc4(const uint8_t* key, size_t keyLen, const uint8_t* in, uint8_t* out, size_t len) {
  uint8_t S[256];
  for (int i = 0; i < 256; i++) S[i] = (uint8_t)i;
  int j = 0;
  for (int i = 0; i < 256; i++) {
    j = (j + S[i] + key[i % keyLen]) & 0xFF;
    uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
  }
  int a = 0, b = 0;
  for (size_t n = 0; n < len; n++) {
    a = (a + 1) & 0xFF;
    b = (b + S[a]) & 0xFF;
    uint8_t t = S[a]; S[a] = S[b]; S[b] = t;
    out[n] = in[n] ^ S[(S[a] + S[b]) & 0xFF];
  }
}

// ── RSA public-key encrypt, PKCS#1 v1.5 ───────────────────────────────────────
static int rngFromEsp(void*, unsigned char* out, size_t len) {
  for (size_t i = 0; i < len; i += 4) {
    uint32_t r = esp_random();
    for (size_t k = 0; k < 4 && i + k < len; k++) out[i + k] = (r >> (8 * k)) & 0xFF;
  }
  return 0;
}

size_t ecspRsaPubEncrypt(const char* pubKeyPem, const uint8_t* in, size_t inLen,
                         uint8_t* out, size_t outCap) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  size_t written = 0;
  // parse_public_key wants the NUL-terminated PEM length including the terminator
  int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)pubKeyPem, strlen(pubKeyPem) + 1);
  if (rc != 0) { ESP_LOGW(TAG, "rsa: parse_public_key failed (-0x%04x)", -rc); mbedtls_pk_free(&pk); return 0; }
  rc = mbedtls_pk_encrypt(&pk, in, inLen, out, &written, outCap, rngFromEsp, nullptr);
  if (rc != 0) { ESP_LOGW(TAG, "rsa: encrypt failed (-0x%04x)", -rc); written = 0; }
  mbedtls_pk_free(&pk);
  return written;
}

// ── self-test ─────────────────────────────────────────────────────────────────
bool omadaDeviceCryptoSelfTest() {
  bool ok = true;

  // Canonical RC4 test vector: key "Key", plaintext "Plaintext" -> BBF316E8D940AF0AD3
  {
    const uint8_t key[] = "Key";     // 3 bytes (drop the NUL)
    const uint8_t pt[]  = "Plaintext";
    uint8_t ct[9];
    ecspRc4(key, 3, pt, ct, 9);
    String got = toHexUpper(ct, 9);
    bool pass = (got == "BBF316E8D940AF0AD3");
    ESP_LOGI(TAG, "selftest RC4        = %s  %s", got.c_str(), pass ? "PASS" : "FAIL(exp BBF316E8D940AF0AD3)");
    ok &= pass;
  }
  // MD5("") = D41D8CD98F00B204E9800998ECF8427E ; SHA256("abc") = BA7816BF8F01CFEA...
  {
    String md5 = ecspMd5Upper((const uint8_t*)"", 0);
    bool p = (md5 == "D41D8CD98F00B204E9800998ECF8427E");
    ESP_LOGI(TAG, "selftest MD5(\"\")    = %s  %s", md5.c_str(), p ? "PASS" : "FAIL");
    ok &= p;
  }
  {
    String sha = ecspSha256Upper((const uint8_t*)"abc", 3);
    bool p = sha.startsWith("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    ESP_LOGI(TAG, "selftest SHA256(abc)= %s  %s", sha.c_str(), p ? "PASS" : "FAIL");
    ok &= p;
  }
  // ecsp2_auth vs the emulator ground truth (the controller-facing digest — must match byte-for-byte).
  {
    String a = ecsp2Auth("admin", "admin", "0123456789abcdef0123456789abcdef0123");
    bool p = (a == "D1D728F6EA0CB2E1F7E008E526822D896F55ED50F11B998CADC99BCF47B84E2D");
    ESP_LOGI(TAG, "selftest ecsp2Auth  = %s  %s", a.c_str(), p ? "PASS" : "FAIL");
    ok &= p;
  }

  ESP_LOGW(TAG, "crypto self-test: %s", ok ? "ALL PASS" : "FAILURES — do not adopt");
  return ok;
}

// ── lifecycle (dormant; transport/discovery/adopt land in the next milestones) ─
void setupOmadaDevice() {
  // Milestone 1 scaffold: verify the crypto is byte-correct on real silicon before
  // any handshake code depends on it. The adopt task is not started yet.
  omadaDeviceCryptoSelfTest();
}
