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

// Checkpoint-based rewind support (via new exports - see kailleraclient.cpp)
// for static local-file Playback only (RB_MODE_PLAYBACK, i.e. player_playing
// && !player_watch_mode) - "Watch Live" streaming has no fixed underlying
// file to seek within, so both of these are no-ops/return -1 while
// player_watch_mode is true.

// The krec_reader's own input-frame count (krec_reader::frame_index()) right
// now - the frontend uses this to space out how often it takes a
// core_serialize() checkpoint (every N frames of actual replay content, not
// wall-clock time, so spacing stays consistent regardless of fast-forward
// speed) and to remember which frame each checkpoint corresponds to. -1 when
// not in static local-file playback.
int player_get_frame_index();

// Forces the krec_reader's read position back to `frame`
// (krec_reader::seek_to_frame()) - called right after the frontend
// core_unserialize()s to an earlier checkpoint, so subsequent player_MPV()
// calls resume feeding input from the matching point in the recording
// instead of wherever the reader happened to be. No-op outside static
// local-file playback.
void player_seek_to_frame(int frame);

// Total input-frame count of the currently open recording (krec_reader::count_total_frames(),
// scanned once when the file is opened - see player_play()), for the
// frontend's on-screen progress bar. -1 when not in static local-file
// playback.
int player_get_total_frames();
