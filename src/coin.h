#pragma once

#include "globals.h"
#include "led.h"
#include "network.h"

void IRAM_ATTR coinButtonPulse();   // debounced — boot button / test
void IRAM_ATTR coinSlotPulse();     // no debounce — real coin acceptor

void masterRecvCoinInserted(const char* senderMac);
void slaveRecvCoinInsertStart();
void slaveRecvCoinInsertEnd();

void coinPulseTask(void*);
void coinSessionTask(void*);   // master-only owner of session state + insertion deadline

// ── Session API (used by web.cpp) ─────────────────────────────────────────────
// coinSessionTryClaim: CAS false→true on COIN_INSERT_ACTIVE; on success posts COIN_SESSION_START.
// coinSessionUpdateSocket: swap in new fd only if the slot's ip still matches (guarded CAS).
// coinSessionSnapshot: one coherent read for the status path.
bool coinSessionTryClaim(int socketFd, const String& clientIp);
bool coinSessionUpdateSocket(const String& clientIp, int socketFd);
CoinSessionStatus coinSessionSnapshot();
uint32_t coinIpToU32(const String& ip);
String   coinU32ToIp(uint32_t v);
