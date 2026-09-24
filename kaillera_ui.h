#pragma once

#include "kcore/kaillera_core.h"

bool kaillera_SelectServerDlgStep();
void kaillera_EndGame();
void kaillera_GUI();
void kaillera_ui_chat_send(char * xxx);

bool kaillera_RecordingEnabled();
bool kaillera_IsHost();
bool kaillera_StreamingEnabled();
void kaillera_ConfigureStream();
void kaillera_GetOwnerName(char* out, int cap);

bool ValidateGameBeforePlay(const char* gameName);


