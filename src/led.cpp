#include "led.h"
#include "globals.h"

#include <esp_log.h>

//static const char* TAG = "led";

extern bool IS_MASTER;

TaskHandle_t LED_TASK = NULL;

#ifdef HAS_RGB_LED
#include <Adafruit_NeoPixel.h>
#define LED_PIN    48  // Built-in WS2812 on ESP32-S3 DevKitC-1
#define LED_COUNT   1
Adafruit_NeoPixel LED_STRIP(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
#endif

void setupLed() {
#ifdef HAS_RGB_LED
  LED_STRIP.begin();
  LED_STRIP.setBrightness(80);
  LED_STRIP.clear();
  LED_STRIP.show();
#endif
  xTaskCreatePinnedToCore(ledTask, "LedTask", 2048, NULL, 2, &LED_TASK, 1);
}

void ledRoleKnown() {
  if (LED_TASK) xTaskNotify(LED_TASK, LED_NOTIFY_ROLE, eSetValueWithOverwrite);
}

void ledReady() {
  if (LED_TASK) xTaskNotify(LED_TASK, LED_NOTIFY_READY, eSetValueWithOverwrite);
}

void ledError() {
  if (LED_TASK) xTaskNotify(LED_TASK, LED_NOTIFY_ERROR, eSetValueWithOverwrite);
}

void ledHalt() {
  digitalWrite(COINSLOT_POWER_PIN, LOW);  // cut coin slot power immediately
#ifdef HAS_RGB_LED
  if (LED_TASK) vTaskSuspend(LED_TASK);  // stop task fighting us for the strip
  LED_STRIP.begin();
  LED_STRIP.setBrightness(80);
  for (;;) {
    LED_STRIP.setPixelColor(0, LED_STRIP.Color(255, 0, 0));
    LED_STRIP.show();
    delay(100);
    LED_STRIP.clear();
    LED_STRIP.show();
    delay(100);
  }
#else
  for (;;);
#endif
}

// Notify LED_TASK with LED_NOTIFY_START to begin blinking, LED_NOTIFY_STOP to end.
// Duplicate signals are naturally idempotent — value overwrites any pending notification.
void ledTask(void*) {
#ifdef HAS_RGB_LED
  uint32_t idleColor = LED_STRIP.Color(255, 255, 255);  // white until role known; updated on READY
  bool booting  = true;   // startup blink: idle ↔ off at 500ms
  bool blinking = false;  // coin-insert blink: green ↔ idle at 200ms
  bool error    = false;  // error blink: red ↔ off at 200ms; cleared by READY
  bool flashOn  = false;
  for (;;) {
    uint32_t val = 0;
    TickType_t timeout = (booting || blinking || error) ? pdMS_TO_TICKS(booting ? 500 : 200) : portMAX_DELAY;
    if (xTaskNotifyWait(0, ULONG_MAX, &val, timeout) == pdTRUE) {
      if (val == LED_NOTIFY_ROLE) {
        idleColor = IS_MASTER ? LED_STRIP.Color(128, 0, 128) : LED_STRIP.Color(255, 80, 0);
      } else if (val == LED_NOTIFY_READY) {
        booting  = false;
        blinking = false;
        error    = false;
        if (!COINSLOT_PROGRAM_MODE) digitalWrite(COINSLOT_POWER_PIN, LOW);
        LED_STRIP.setPixelColor(0, idleColor);
        LED_STRIP.show();
      } else if (val == LED_NOTIFY_START) {
        booting  = false;
        blinking = true;
        error    = false;
        flashOn  = false;
        if (!COINSLOT_PROGRAM_MODE) digitalWrite(COINSLOT_POWER_PIN, HIGH);
      } else if (val == LED_NOTIFY_STOP) {
        blinking = false;
        if (!COINSLOT_PROGRAM_MODE) digitalWrite(COINSLOT_POWER_PIN, LOW);
        LED_STRIP.setPixelColor(0, idleColor);
        LED_STRIP.show();
      } else if (val == LED_NOTIFY_ERROR) {
        booting  = false;
        blinking = false;
        error    = true;
        flashOn  = false;
      }
    } else {
      // timeout — toggle
      flashOn = !flashOn;
      if (blinking) {
        LED_STRIP.setPixelColor(0, flashOn ? LED_STRIP.Color(0, 255, 0) : idleColor);
      } else if (error) {
        LED_STRIP.setPixelColor(0, flashOn ? LED_STRIP.Color(255, 0, 0) : 0);
      } else {  // booting
        LED_STRIP.setPixelColor(0, flashOn ? idleColor : 0);
      }
      LED_STRIP.show();
    }
  }
#else
  vTaskDelete(NULL);  // no LED hardware — nothing to do
#endif
}
