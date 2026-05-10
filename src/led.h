#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define LED_NOTIFY_READY 0  // end startup blink → solid idle
#define LED_NOTIFY_START 1  // begin coin-insert blink
#define LED_NOTIFY_STOP  2  // end coin-insert blink → solid idle
#define LED_NOTIFY_ERROR 3  // blink red until READY clears it

extern TaskHandle_t LED_TASK;  // notify with LED_NOTIFY_START to begin blinking, LED_NOTIFY_STOP to end

void ledSetup();      // initialize strip with dim init color; call after role is known
void ledReady();      // switch to full idle color; call at end of setup()
void ledError();      // begin red blink — cleared by ledReady() on recovery
void ledTask(void*);  // FreeRTOS task — notify to start blinking, notify again to stop
void ledHalt();       // fast red blink forever — safe before or after task starts; never returns
