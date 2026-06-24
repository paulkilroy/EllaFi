#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define LED_NOTIFY_READY 0  // end startup blink → solid idle
#define LED_NOTIFY_START 1  // begin coin-insert blink
#define LED_NOTIFY_STOP  2  // end coin-insert blink → solid idle
#define LED_NOTIFY_ERROR 3  // blink red until READY clears it
#define LED_NOTIFY_ROLE  4  // role known — switch blink color to master/slave; keeps booting blink

extern TaskHandle_t LED_TASK;

void setupLed();       // initialize strip, start LED task
void ledRoleKnown();   // switch boot blink to role color; call after IS_MASTER is set
void ledReady();       // end boot blink → solid idle; call at end of setup()
void ledError();       // begin red blink — cleared by ledReady() on recovery
void ledTask(void*);
void ledHalt();        // fast red blink forever — never returns

// Direct strip control for a blocking owner (e.g. WiFi recovery). ledTakeOver() suspends LED_TASK
// so it stops fighting for the strip; ledShow() then drives the single pixel. No-ops without RGB.
void ledTakeOver();
void ledShow(uint8_t r, uint8_t g, uint8_t b);
