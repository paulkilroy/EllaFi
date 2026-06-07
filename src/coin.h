#pragma once

#include "globals.h"
#include "led.h"
#include "network.h"

void IRAM_ATTR coinButtonPulse();      // debounced — boot button / test
void IRAM_ATTR coinSlotPulse();  // no debounce — real coin acceptor
void coinTimerCallback(TimerHandle_t xTimer);
void restartCoinInsertTimer();

void masterRecvCoinInserted(const char* senderMac);
void slaveRecvCoinInsertStart();
void slaveRecvCoinInsertEnd();

void coinPulseTask(void*);
void finalizeCoinTask(void*);
void finalizeCoinInsert();
