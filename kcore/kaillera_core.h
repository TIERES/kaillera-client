#pragma once

#include "../common/n02_version.h"

#define N02_COMP_VER "kaillera 0.9"
#define KAILLERA_VERSION N02_VERSION " (" N02_COMP_VER " compatible)"

int kaillera_ping_server(char * host, int port, int limit = 1000);
void kaillera_step();


void __cdecl kaillera_core_debug(char * arg_0, ...);
void __cdecl kaillera_error_callback(char * arg_0, ...);


int kaillera_get_frames_count();
int kaillera_get_delay();

bool kaillera_is_connected();
bool kaillera_is_host();
void kaillera_get_username(char* out, int cap);
bool kaillera_core_initialize(int port, char * appname, char * username, char connection_setting);
void kaillera_set_spoof_ping(int spoof_ping_ms);  // Call before connect: 0=auto, >0=spoof ping in ms
bool kaillera_core_connect(char * ip, int port = 27888);
bool kaillera_disconnect(char * quitmsg);
bool kaillera_core_cleanup();
int kaillera_core_get_port();
void kaillera_chat_send(char * text);
void kaillera_game_chat_send(char * text);
void kaillera_kick_user (unsigned short id);
void kaillera_join_game(unsigned int id);
void kaillera_create_game(char * name);
void kaillera_leave_game ();
void kaillera_start_game();
void kaillera_game_drop();
void kaillera_end_game();
int kaillera_modify_play_values (void * values, int size);
void kaillera_print_core_status();
bool kaillera_is_game_running();

// retry-connect - resume a dropped match from a recorded .krec as a
// synchronized group replay, navigated with RetroArch's own native Pause key
// (host only) and committed to live play with Enter (also host only, valid
// only while paused). Wire sub-protocol (RETRYCON_SELECT/CONTROL/NAK,
// defined in kcore/k_instruction.h) is transport-only here, same as every
// other instruction type - the actual state machine (download orchestration,
// feeding the emulator from the local recording, relaying RetroArch's
// pause/resume/go-live) lives in kaillera_retryconnect.cpp, which implements
// the *_callback functions below.
void kaillera_retryconnect_send_select(const char* session_id, unsigned short session_id_len);
void kaillera_retryconnect_send_control(int action, int frame_index);
void kaillera_retryconnect_send_nak();

// fromUser is the sender's username (never our own - the server doesn't echo
// notifications back to their sender, see RetryConnectAction.kt). session_id
// is NOT NUL-terminated on the wire; kaillera_core.cpp copies it into a
// NUL-terminated buffer no larger than 63 chars before calling back.
void kaillera_retryconnect_select_callback(char* fromUser, char* session_id);
void kaillera_retryconnect_control_callback(char* fromUser, int action, int frame_index);
void kaillera_retryconnect_nak_callback(char* fromUser);

// Non-blocking drain of pending instructions, RETRYCON included - no-op
// (false, no socket work) when there's no retry-connect session active.
// kaillera_modify_play_values() already calls this every live frame; the
// frontend's kailleraRetryConnectPoll() export also needs to call it
// directly, because kailleraModifyPlayValues() itself stops being called at
// all while the frontend is natively paused (RetroArch's runloop skips
// core_run() - see the project's retry-connect design notes) - without this,
// a RESUME/GO_LIVE sent while a peer is already paused would never be seen.
bool kaillera_retryconnect_pump();

void kaillera_user_add_callback(char*name, int ping, int status, unsigned short id, char conn);
void kaillera_game_add_callback(char*gname, unsigned int id, char*emulator, char*owner, char*users, char status);
void kaillera_chat_callback(char*name, char * msg);
void kaillera_game_chat_callback(char*name, char * msg);
void kaillera_motd_callback(char*name, char * msg);
void kaillera_user_join_callback(char*name, int ping, unsigned short id, char conn);
void kaillera_user_leave_callback(char*name, char*quitmsg, unsigned short id);
void kaillera_game_create_callback(char*gname, unsigned int id, char*emulator, char*owner);
void kaillera_user_game_close_callback();
void kaillera_game_close_callback(unsigned int id);
void kaillera_user_game_create_callback();
void kaillera_game_status_change_callback(unsigned int id, char status, int players, int maxplayers);
void kaillera_user_game_closed_callback();
void kaillera_user_game_close_callback();
void kaillera_user_game_closed_callback();
void kaillera_user_game_closed_callback();
void kaillera_user_game_joined_callback();
void kaillera_player_add_callback(char *name, int ping, unsigned short id, char conn);
void kaillera_player_joined_callback(char * username, int ping, unsigned short uid, char connset);
void kaillera_player_left_callback(char * user, unsigned short id);
void kaillera_user_kicked_callback();
void kaillera_login_stat_callback(char*lsmsg);
void kaillera_duplicate_username_callback(char* conflictingName);
void kaillera_player_dropped_callback(char * user, int gdpl);
void kaillera_game_callback(char * game, char player, char players);
void kaillera_game_netsync_wait_callback(int tx);
void kaillera_end_game_callback();
