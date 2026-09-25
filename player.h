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

// Starts spectating immediately, without switching active_mod (so the caller
// stays wherever it is - Kaillera mode's own server/lobby connection keeps
// running unmodified). Fetches the stream, parses its KRC1 header and forces
// KSSDFA into "game running" (the same local-only trick player_play() uses -
// no Kaillera server round trip needed), same as player_watch_begin() always
// did internally; only the caller changed - see kaillera_ui.cpp's
// kaillera_sdlg_watch_selected_game(), which calls this directly instead of
// the request+activate_mode(2) dance above. kaillera_modify_play_values()
// (kcore/kaillera_core.cpp) delegates to player_MPV() whenever
// player_is_watching() is true, regardless of which module is "active".
// Returns false (and already showed its own error MessageBox) on failure -
// caller should bail out without announcing anything.
bool player_watch_begin(const char* sessionId, const char* roomName);

// True once player_watch_begin() has succeeded and until the stream/watch
// ends (player_EndGame()) - kaillera_modify_play_values() and kailleraEndGame()
// both check this to route to player_MPV()/player_EndGame() instead of their
// own normal Kaillera-mode logic.
bool player_is_watching();

// The up-to-4 player names read from the watched stream's KRC1 header
// (recording_player_names, see kailleraclient.cpp's recording writer) -
// empty strings for unused slots. Only meaningful right after a successful
// player_watch_begin(); for the "who's playing" chat announcement in
// kaillera_sdlg_watch_selected_game().
void player_watch_get_player_names(char out[4][32]);

// True if a state has been sent during Watch Live (via kailleraWatchJumpToLive()),
// false after pause/rewind or when not in Watch Live mode.
bool player_watch_state_was_sent();

// "Ir direto para o Ao Vivo!" - call right after the frontend applies a
// state downloaded via the DLL's kailleraWatchDownloadState() export
// (core_unserialize()), passing back the same frameIndex/byteOffset that
// call returned. Discards whatever was locally buffered and resumes
// fetching from byteOffset in the host's stream - see this function's own
// comment in player.cpp for why a straight rewind-style seek isn't enough
// here. No-op outside Watch Live.
void player_watch_jump_to_live(int frameIndex, int byteOffset);

// Set by kaillera_ui.cpp to be notified when watch mode ends (stream ended,
// or the user stopped) - player_EndGame() calls it (if non-NULL) right after
// clearing its own watch-mode state, so the caller can undo any UI it changed
// while watching (see kaillera_sdlg_watch_selected_game()'s local Status
// override). NULL by default - Playback mode itself doesn't need it.
extern void (*player_watch_ended_callback)();

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
