#include "omada_device.h"
#include "globals.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <esp_random.h>
#include <esp_log.h>
#include "logger.h"   // must be included LAST — see logger.h

static const char* TAG = "omadaDev";

extern String CONTROLLER_BASE_URL;   // "https://host:port" — from omada.cpp/config
extern bool   OMADA_DEVICE_ENABLE;   // config: omada_device_enable

// ── device identity (discovery-safe: model name is public; hwId/oemId secrets are
// only needed at adopt-time, added later from NVS). ───────────────────────────
static const char* DEV_MODEL   = "EAP225-Outdoor";
static const char* DEV_PROTOVER = "2.3.0";                 // ECSP protocol version (NOT firmware)
static const char* DEV_CTRLID  = "c21f969b5f03d33d43e04f8f136e7682";  // md5("default") = factory→PENDING
static const int   ECSP_DISCOVERY_PORT = 29810;

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

// ── discovery beacon (UDP :29810, plaintext ECSP frame) ───────────────────────
// Wire format (omada-lab/discovery_beacon.py): [4-byte BE len][JSON {header,body}].
// type=1 DISCOVERY; controllerId=md5("default") → the controller flags us factory → PENDING.
static String controllerHost() {
  // CONTROLLER_BASE_URL = "https://HOST:PORT" → HOST
  int a = CONTROLLER_BASE_URL.indexOf("://");
  if (a < 0) return "";
  a += 3;
  int b = CONTROLLER_BASE_URL.indexOf(':', a);
  int c = CONTROLLER_BASE_URL.indexOf('/', a);
  int end = CONTROLLER_BASE_URL.length();
  if (b >= 0) end = min(end, b);
  if (c >= 0) end = min(end, c);
  return CONTROLLER_BASE_URL.substring(a, end);
}

static String buildDiscoveryFrame() {
  String mac = WiFi.macAddress(); mac.toUpperCase();
  String ip  = WiFi.localIP().toString();
  uint32_t total = ESP.getHeapSize();
  int memUti = total ? (int)(100 - (uint64_t)ESP.getFreeHeap() * 100 / total) : 40;

  // hand-built to control key order/types exactly (types matter — see discovery_beacon.py)
  String h = String("{\"seq\":1,\"version\":\"") + DEV_PROTOVER + "\",\"device\":\"ap\",\"mac\":\""
           + mac + "\",\"type\":1,\"timestamp\":" + String((long)time(NULL)) + ",\"ip\":\"" + ip + "\"}";
  String di = String("{\"name\":\"EllaFi-") + mac.substring(12) + "\",\"model\":\"" + DEV_MODEL
           + "\",\"modelVersion\":\"1.0\",\"firmwareVersion\":\"1.0.0 Build 20260101 Rel.00000\","
           + "\"hardwareVersion\":\"1.0\",\"upTime\":\"" + String(millis()/1000) + "\",\"cpuUti\":5,\"memUti\":"
           + String(memUti) + ",\"wirelessLinked\":false}";
  String dm = "{\"support_11ac\":true,\"support_lag\":false,\"supportMesh\":1,\"customizeRegion\":0,"
              "\"lanPortsNum\":1,\"support_channelLimit\":false,\"supportDfs\":0,\"supportRoaming\":1,"
              "\"countryCode\":1}";
  String body = String("{\"deviceInfo\":") + di + ",\"deviceMisc\":" + dm
              + ",\"controllerSetting\":{\"controllerId\":\"" + DEV_CTRLID + "\",\"destOmadacId\":\"\"}}";
  String msg = String("{\"header\":") + h + ",\"body\":" + body + "}";

  String frame; frame.reserve(msg.length() + 4);
  uint32_t n = msg.length();
  frame += (char)(n >> 24); frame += (char)(n >> 16); frame += (char)(n >> 8); frame += (char)n;
  frame += msg;
  return frame;
}

static void omadaDeviceTask(void*) {
  WiFiUDP udp;
  IPAddress bcast(255, 255, 255, 255);
  ESP_LOGW(TAG, "device plane ENABLED — discovery beacon to %s:%d + broadcast, every 30s",
           controllerHost().c_str(), ECSP_DISCOVERY_PORT);
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      String frame = buildDiscoveryFrame();
      String host = controllerHost();
      // L3-directed to the controller (proven path) + subnet broadcast (real-AP path)
      if (host.length()) {
        udp.beginPacket(host.c_str(), ECSP_DISCOVERY_PORT);
        udp.write((const uint8_t*)frame.c_str(), frame.length());
        udp.endPacket();
      }
      udp.beginPacket(bcast, ECSP_DISCOVERY_PORT);
      udp.write((const uint8_t*)frame.c_str(), frame.length());
      udp.endPacket();
      ESP_LOGD(TAG, "discovery beacon sent (%d bytes)", frame.length());
    }
    vTaskDelay(pdMS_TO_TICKS(30000));   // context times out ~40s without a beacon
  }
}

// ── lifecycle ─────────────────────────────────────────────────────────────────
void setupOmadaDevice() {
  omadaDeviceCryptoSelfTest();   // verify crypto is byte-correct before any handshake depends on it
  if (!OMADA_DEVICE_ENABLE) { ESP_LOGI(TAG, "device plane disabled (omada_device_enable=false)"); return; }
  xTaskCreatePinnedToCore(omadaDeviceTask, "omadaDev", 6144, nullptr, 1, nullptr, 0);
}
