#include "led.h"

#include <esp_log.h>

static const char* TAG = "led";

extern bool IS_MASTER;

TaskHandle_t LED_TASK = NULL;

#ifdef HAS_RGB_LED
#include <Adafruit_NeoPixel.h>
#define LED_PIN    48  // Built-in WS2812 on ESP32-S3 DevKitC-1
#define LED_COUNT   1
Adafruit_NeoPixel LED_STRIP(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
#endif

void ledSetup() {
#ifdef HAS_RGB_LED
  LED_STRIP.begin();
  LED_STRIP.setBrightness(80);
  uint32_t initColor = IS_MASTER ? LED_STRIP.Color(40, 0, 40) : LED_STRIP.Color(80, 80, 0);
  LED_STRIP.setPixelColor(0, initColor);
  LED_STRIP.show();
#endif
}

void ledReady() {
#ifdef HAS_RGB_LED
  uint32_t idleColor = IS_MASTER ? LED_STRIP.Color(128, 0, 128) : LED_STRIP.Color(255, 80, 0);
  LED_STRIP.setPixelColor(0, idleColor);
  LED_STRIP.show();
#endif
}

// Notify LED_TASK with LED_NOTIFY_START to begin blinking, LED_NOTIFY_STOP to end.
// Duplicate signals are naturally idempotent — value overwrites any pending notification.
void ledTask(void*) {
#ifdef HAS_RGB_LED
  uint32_t idleColor = IS_MASTER ? LED_STRIP.Color(128, 0, 128) : LED_STRIP.Color(255, 80, 0);
  bool blinking = false;
  bool flashOn  = false;
  for (;;) {
    uint32_t val = 0;
    TickType_t timeout = blinking ? pdMS_TO_TICKS(200) : portMAX_DELAY;
    if (xTaskNotifyWait(0, ULONG_MAX, &val, timeout) == pdTRUE) {
      if (val == LED_NOTIFY_START && !blinking) {
        blinking = true;
        flashOn  = false;
      } else if (val == LED_NOTIFY_STOP && blinking) {
        blinking = false;
        LED_STRIP.setPixelColor(0, idleColor);
        LED_STRIP.show();
      }
    } else if (blinking) {
      // 200ms timeout — toggle flash
      flashOn = !flashOn;
      LED_STRIP.setPixelColor(0, flashOn ? LED_STRIP.Color(0, 255, 0) : idleColor);
      LED_STRIP.show();
    }
  }
#else
  vTaskDelete(NULL);  // no LED hardware — nothing to do
#endif
}
