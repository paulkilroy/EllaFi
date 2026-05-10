#pragma once

#include "globals.h"
#include "coin.h"
#include "files.h"
#include <PsychicFileResponse.h>

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
void processPause(const String& mac, OmadaSession& op);
void processResume(const String& mac, OmadaSession& op, OmadaSession& admin);
