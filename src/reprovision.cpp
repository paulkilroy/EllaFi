#include "reprovision.h"
#include "globals.h"
#include "led.h"
#include "files.h"

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <esp_log.h>
// PsychicHttpServer comes via globals.h — reuse the project's web stack rather than pull in Arduino
// WebServer.h, which redefines HTTP_ANY / HTTPAuthMethod and clashes with PsychicHttp.

static const char* TAG = "reprovision";

static constexpr unsigned long WIFI_RECOVERY_RETRY_MILLIS = 10000;            // re-attempt stored creds
static constexpr unsigned long REPROVISION_HOLD_MILLIS     = 3000;            // BOOT-button hold to arm SoftAP
static constexpr unsigned long PROVISION_IDLE_MILLIS       = 5UL * 60000UL;   // close after 5 min idle
static constexpr unsigned long PROVISION_MAX_MILLIS        = 15UL * 60000UL;  // hard cap

// Renders the SoftAP config page: every current config key as a pre-filled input (passwords masked).
static String provisionFormHtml() {
  JsonDocument doc;
  deserializeJson(doc, maskedConfigJson());
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<meta name=viewport content='width=device-width,initial-scale=1'>"
                  "<title>EllaFi Setup</title></head>"
                  "<body style='font-family:sans-serif;max-width:480px;margin:auto;padding:1em'>"
                  "<h2>EllaFi Node Setup</h2><p>Update the fields that changed, then Save.</p>"
                  "<form method=POST action=/save>");
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (kv.value().is<JsonObject>() || kv.value().is<JsonArray>()) continue;  // skip environments{} etc.
    String key = kv.key().c_str(), val = kv.value().as<String>();
    html += "<label>" + key + "</label><br>"
            "<input name=\"" + key + "\" value=\"" + val + "\" style='width:100%;margin-bottom:.7em'><br>";
  }
  html += F("<button style='width:100%;padding:.8em;font-size:1em'>Save &amp; Reboot</button></form></body></html>");
  return html;
}

// BOOT-button hold armed this: bring up an open SoftAP + captive portal so an operator can re-enter
// config (full form — WiFi, Omada, etc.) on any phone with no app and no IP to type. Physical gate
// only (the node is in a locked box); security1-grade channel auth isn't used. Returns to the recovery
// loop on timeout; a successful save restarts the device and never returns.
// POST /save — pull each known config key from the submitted form (ignoring anything else), then run
// it through the shared merge (skips masked secrets, preserves numeric types, validates). On success
// the device restarts; this handler never returns in that case.
static esp_err_t handleProvisionSave(PsychicRequest* request, PsychicResponse* response) {
  JsonDocument cur;
  deserializeJson(cur, maskedConfigJson());
  JsonDocument doc;
  for (JsonPair kv : cur.as<JsonObject>()) {
    const char* k = kv.key().c_str();
    if (request->hasParam(k)) doc[k] = request->getParam(k)->value();
  }
  String body; serializeJson(doc, body);

  String err;
  if (mergeAndSaveConfig(body, err)) {
    ledShow(0, 255, 0);                    // green = saved
    response->send(200, "text/html", "<html><body style='font-family:sans-serif'>"
                                     "<h3>Saved. Rebooting&hellip;</h3></body></html>");
    delay(1000);
    esp_restart();                         // never returns
  }
  return response->send(400, "text/html", ("<html><body style='font-family:sans-serif'><h3>Error: " +
                                          err + "</h3><a href=/>Back</a></body></html>").c_str());
}

static void enterSoftApProvisioning() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char ssid[24]; snprintf(ssid, sizeof(ssid), "EllaFi-Setup-%02X%02X", mac[4], mac[5]);
  ESP_LOGW(TAG, "Provisioning armed — SoftAP '%s'", ssid);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid);                       // open AP — physical (locked-box) gate, no PoP
  IPAddress apIP = WiFi.softAPIP();        // 192.168.4.1

  DNSServer dns;
  dns.start(53, "*", apIP);                // wildcard DNS → captive-portal auto-popup

  PsychicHttpServer server;
  server.config.max_open_sockets = 7;
  server.on("/", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res) {
    return res->send(200, "text/html", provisionFormHtml().c_str());
  });
  server.on("/save", HTTP_POST, handleProvisionSave);
  String portal = "http://" + apIP.toString() + "/";
  server.onNotFound([portal](PsychicRequest* req, PsychicResponse* res) {  // any other URL → portal (captive UI)
    res->addHeader("Location", portal.c_str());
    return res->send(302, "text/plain", "");
  });
  server.begin();

  unsigned long start = millis(), lastActive = millis();
  for (;;) {                               // HTTP is served on its own task; we just pump DNS + LED + timeout
    dns.processNextRequest();
    if (WiFi.softAPgetStationNum() > 0) {  // phone connected → solid blue, keep the window open
      ledShow(0, 0, 255);
      lastActive = millis();
    } else {
      ledShow(0, 0, (millis() / 400) % 2 ? 255 : 0);  // armed & listening → blue blink
    }
    if (millis() - lastActive > PROVISION_IDLE_MILLIS || millis() - start > PROVISION_MAX_MILLIS) break;
    delay(10);
  }

  ESP_LOGW(TAG, "Provisioning window closed — back to WiFi recovery");
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

void runWifiRecovery() {
  ESP_LOGW(TAG, "WiFi unavailable with stored credentials — entering recovery (solid red)");

  // The GPIO0 button ISR posts to COIN_PULSE_QUEUE, which isn't created until later in boot. Detach
  // it and poll the pin directly so a press here can't xQueueSendFromISR(NULL) and crash.
  detachInterrupt(digitalPinToInterrupt(COINBUTTON_PIN));

  ledTakeOver();
  ledShow(255, 0, 0);  // solid red = WiFi down, provisioning available

  WiFi.begin(MANAGEMENT_SSID.c_str(), MANAGEMENT_PASSWORD.c_str());
  unsigned long lastRetry  = millis();
  unsigned long pressStart = 0;

  for (;;) {
    // Background retry of the real credentials — a transient outage self-heals via a clean reboot.
    if (WiFi.status() == WL_CONNECTED) {
      ESP_LOGI(TAG, "WiFi recovered — restarting into normal boot");
      delay(200);
      esp_restart();
    }
    if (millis() - lastRetry > WIFI_RECOVERY_RETRY_MILLIS) {
      lastRetry = millis();
      ESP_LOGW(TAG, "Still no WiFi — retrying %s", MANAGEMENT_SSID.c_str());
      WiFi.begin(MANAGEMENT_SSID.c_str(), MANAGEMENT_PASSWORD.c_str());
    }

    // BOOT button (GPIO0, INPUT_PULLUP → LOW when pressed) held REPROVISION_HOLD_MILLIS arms the
    // SoftAP. Fast red pulse while held = "registered, keep holding."
    if (digitalRead(COINBUTTON_PIN) == LOW) {
      if (pressStart == 0) pressStart = millis();
      ledShow((millis() / 150) % 2 ? 255 : 0, 0, 0);
      if (millis() - pressStart >= REPROVISION_HOLD_MILLIS) {
        enterSoftApProvisioning();
        pressStart = 0;
        ledShow(255, 0, 0);                 // back to solid red after the provisioning window
        WiFi.begin(MANAGEMENT_SSID.c_str(), MANAGEMENT_PASSWORD.c_str());
        lastRetry = millis();
      }
    } else if (pressStart != 0) {
      pressStart = 0;
      ledShow(255, 0, 0);                   // released before 3s → back to solid red
    }

    delay(50);
  }
}
