#pragma once

#include "kcore/kaillera_core.h"

bool kaillera_SelectServerDlgStep();
void kaillera_EndGame();
void kaillera_GUI();
void kaillera_ui_chat_send(char * xxx);

bool kaillera_RecordingEnabled();
bool kaillera_IsHost();
bool kaillera_StreamingEnabled();
bool kaillera_NoMemcardEnabled();
bool kaillera_MultiTapEnabled();
bool kaillera_McOnlineEnabled();
int kaillera_GetGamePlayers(char out[][32], int max);
void kaillera_ConfigureStream();
void kaillera_GetOwnerName(char* out, int cap);

bool ValidateGameBeforePlay(const char* gameName);


