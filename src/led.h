#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define LED_NOTIFY_START 1
#define LED_NOTIFY_STOP  2

extern TaskHandle_t LED_TASK;  // notify with LED_NOTIFY_START to begin blinking, LED_NOTIFY_STOP to end

void ledSetup();      // initialize strip with dim init color; call after role is known
void ledReady();      // switch to full idle color; call at end of setup()
void ledTask(void*);  // FreeRTOS task — notify to start blinking, notify again to stop
