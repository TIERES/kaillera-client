#pragma once

#include "kcore/kaillera_core.h"

bool kaillera_SelectServerDlgStep();
void kaillera_EndGame();
void kaillera_GUI();
void kaillera_ui_chat_send(char * xxx);

bool kaillera_RecordingEnabled();
bool kaillera_IsHost();
bool kaillera_StreamingEnabled();
bool kaillera_MultiTapEnabled();
// The memory card of the game that started, slot 1 and 2
// (N02_MEMCARD_CHOICE_*, common/n02_mcchoice.h).
void kaillera_GetGameMcSlots(int slot[2]);
int kaillera_GetGamePlayers(char out[][32], int max);
void kaillera_ConfigureStream();
void kaillera_GetOwnerName(char* out, int cap);
// The connected server's name (as in the server list) - or its address.
void kaillera_GetServerName(char* out, int cap);

bool ValidateGameBeforePlay(const char* gameName);


