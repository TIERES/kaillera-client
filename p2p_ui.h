#pragma once

#include <windows.h>
#include "core/p2p_core.h"

bool p2p_SelectServerDlgStep();
void p2p_EndGame();
void p2p_GUI();
void p2p_ui_chat_send(char * xxx);
bool p2p_RecordingEnabled();
bool p2p_IsHost();
bool p2p_StreamingEnabled();
// The memory card of the game starting now, slot 1 and 2
// (N02_MEMCARD_CHOICE_*, common/n02_mcchoice.h).
void p2p_GetGameMcSlots(int slot[2]);
int p2p_GetGamePlayers(char out[][32], int max);
void p2p_ConfigureStream();
void p2p_GetOwnerName(char* out, int cap);
void p2p_ShowWaitingGamesList();
void InitializeP2PSubsystem(HWND hDlg, bool host);
void InitializeP2PSubsystem(HWND hDlg, bool host, bool hostByCode);
void InitializeP2PSubsystemWithJoinCode(HWND hDlg, const char* join_code, const char* join_fallback_ip_port);
