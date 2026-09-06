#pragma once
// ── Omada device plane (fake-AP) — Phase B, Milestone 1 ───────────────────────
// Ports omada-lab/emulator to firmware so each EllaFi node appears as a managed
// EAP225-Outdoor in the operator's Omada controller (health, logs, alerts, OTA).
// Design: omada-lab/omada_integration_design.md. Wire contract:
// omada-lab/omada-protocol-reference.md (crypto §2, handshake §4).
//
// DECOUPLED from the coin/auth/web paths: outbound-only, best-effort, its own
// low-priority task. Never blocks boot or selling. Off unless enabled in config.
//
// This header exposes the crypto primitives too, so they can be self-tested
// against the emulator's known vectors on real hardware.

#include <Arduino.h>
#include <stdint.h>

// ── Crypto (faithful to the controller's own impl — digests are compared) ─────
// All hex output UPPERCASE (CipherUtils.bytesToHexString).

String ecspMd5Upper(const uint8_t* data, size_t len);
String ecspSha256Upper(const uint8_t* data, size_t len);

// EcspUtils.calculateEcsp2Auth: SHA256_UPPER( SHA256_UPPER(username + MD5_UPPER(password)) + randomKey ).
// Factory device proves admin/admin before the controller pushes the site account.
String ecsp2Auth(const String& username, const String& password, const String& randomKey);
// Variant when the caller already holds MD5(password) as UPPER hex (ADOPT_REQUEST delivers it).
String ecsp2AuthMd5(const String& username, const String& md5UpperHex, const String& randomKey);

// Standard RC4 (RC4Utils.initKey/doCrypt): KSA with key-repeat + PRGA. In-place-safe.
void ecspRc4(const uint8_t* key, size_t keyLen, const uint8_t* in, uint8_t* out, size_t len);

// RSA public-key encrypt, PKCS#1 v1.5 (RsaCipher "RSA/ECB/PKCS1Padding") — the device wraps its
// random RC4 session key with the controller's netty PUBLIC key. `pubKeyPem` is a device-only
// constant (loaded from NVS / gitignored FS, NEVER committed). Returns bytes written, 0 on error.
size_t ecspRsaPubEncrypt(const char* pubKeyPem, const uint8_t* in, size_t inLen,
                         uint8_t* out, size_t outCap);

// Runs the crypto self-tests (canonical RC4 vector + MD5/SHA256 knowns). Logs PASS/FAIL.
bool omadaDeviceCryptoSelfTest();

// ── Lifecycle ─────────────────────────────────────────────────────────────────
// Runs the crypto self-test; if OMADA_DEVICE_ENABLE, starts omadaDeviceTask (the
// discovery beacon → adoption → inform loop). Best-effort; never blocks boot/selling.
void setupOmadaDevice();   // call after WiFi + web are up.
