#pragma once

int player_MPV(void*,int);
void player_GUI();
void player_EndGame();
bool player_SSDSTEP();
void player_ChatSend(char*);
bool player_RecordingEnabled();

// Requests that the playback module start spectating a live "Watch Live"
// session (sessionId from n02_watch_lookup_session()) the next time its GUI
// opens - called from kaillera_ui.cpp's lobby "Watch" menu just before
// activate_mode(2) switches into playback mode.
void player_request_watch(const char* sessionId, const char* roomName);
