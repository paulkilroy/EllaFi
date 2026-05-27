#pragma once

#include "globals.h"
#include "coin.h"
#include "files.h"
#include "embedded_files.h"

void      setupOtaRoute();
void      setupFsOtaRoute();
esp_err_t handleFwBin(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleAdminPage(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleAdminNodes(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleAdminLog(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleAdminSellers(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleAdminVouchers(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleAdminConfig(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleAdminReboot(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleRoot(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleGetStatus(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleNotFound(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleRefunds(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleErrors(PsychicRequest* request, PsychicResponse* response);
esp_err_t handleProgram(PsychicRequest* request, PsychicResponse* response);
void handleWsOpen(PsychicWebSocketClient* client);
esp_err_t handleWsRequest(PsychicWebSocketRequest* request, httpd_ws_frame* frame);
void handleWsClose(PsychicWebSocketClient* client);

String buildStatusJson(const SessionParams& session, const char* type = "status");
void pushCoinUpdateToClient();

void updateClientTask(void*);
void webSocketTask(void*);
void processPause(const String& mac);
void processResume(const String& mac);
