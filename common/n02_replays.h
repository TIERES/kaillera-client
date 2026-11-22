/******************************************************************************
Online replay browser - the "Replays Online" checkbox in the Playback screen.

Lets the user browse the community server's finished recordings (matches
recorded via n02_stream.h that ran at least 5 minutes - see wg-camp's
GET /replays/list.txt) without leaving the emulator, and download one
straight into .\records\ (optionally starting playback right away).
******************************************************************************/
#pragma once

#define N02_REPLAYS_MAX_ENTRIES 20

typedef struct {
	char session_id[64];
	char when[24];        // "DD-MM-YYYY HH:MM", already formatted server-side
	char game_name[128];
	char player_names[160];
	int  duration_seconds;
	char download_name[200]; // safe filename (no path) suggested by the server
	int  size_bytes;
} N02ReplayEntry;

// Fetches the most recent replays (newest first) via a blocking GET against
// the community server's /replays/list.txt, up to maxEntries (itself capped
// at N02_REPLAYS_MAX_ENTRIES). Returns how many were parsed into out, or 0 on
// any network/parse failure. Call off the UI thread's message loop if
// possible - this blocks for the network round trip (bounded by a short
// timeout).
int n02_replays_fetch_list(N02ReplayEntry* out, int maxEntries);

// Reconectar: the newest replays (up to maxEntries) between exactly these
// players - playersCsv is the room's nicks joined with "," - and, when game is
// non-empty, of this exact game. The server filters before applying the limit,
// so an old match between them still shows up after many other games on the
// server. Returns how many were parsed, 0 when there are none, or -1 on any
// network/HTTP failure. An older server ignores the filter and returns its
// newest replays, so the caller should still check each entry against the room.
int n02_replays_fetch_list_between(N02ReplayEntry* out, int maxEntries, const char* playersCsv, const char* game);

// Downloads session `sessionId`'s .krec via a blocking GET against
// /replays/<sessionId>/download, writing it straight to destPath (overwriting
// any existing file). Returns true on success. Like n02_replays_fetch_list,
// this blocks for the network transfer - the file can be a few hundred KB to
// a few MB, so this is not instant on a slow link.
bool n02_replays_download(const char* sessionId, const char* destPath);

// retry-connect: the host uploads a savestate the instant it stops
// fast-forwarding a group replay, so every other client can jump straight to
// the exact same frame instead of trying to reach it by replaying
// frame-by-frame at whatever speed their own machine/core allows (see
// kcore/kaillera_retryconnect.h). Blocking POST to
// /replays/<sessionId>/state; only the latest state matters; returns true on
// success.
bool n02_replays_upload_state(const char* sessionId, int frameIndex, const void* data, int dataSize);

// Companion to n02_replays_upload_state() - blocking GET of the same state.
// On success returns a malloc()'d buffer (caller frees it) holding just the
// savestate bytes (the wire-format frame index header is stripped) and fills
// *outFrameIndex/*outSize; returns NULL on any failure, including "host
// hasn't uploaded one yet" (404).
void* n02_replays_download_state(const char* sessionId, int* outFrameIndex, int* outSize);

// "Ir direto para o Ao Vivo!" (kaillera-client's Watch Live toolbar) - same
// idea as the three functions above, but for a still-*live* spectate session
// (see n02_watch.h) rather than a finished replay: POST/GET
// /spectate/<sessionId>/state-request and /spectate/<sessionId>/state
// instead of /replays/<sessionId>/state (spectate.py gates those on
// "finished", these on just "known session id"). n02_watch.cpp's own
// n02_watch_request_state()/state_ready()/download_state() are thin
// wrappers around these three, supplying whatever session n02_watch_start()
// is currently pointed at - call those instead of these directly.

bool n02_replays_request_live_state(const char* sessionId);

// True once the host has serviced the pending request (server-side flag
// cleared) - see n02_replays_request_live_state() above.
bool n02_replays_live_state_ready(const char* sessionId);

// Companion download - unlike n02_replays_download_state() above, the wire
// header here is 8 bytes ([frame_index:int32 LE][byte_offset:int32 LE], see
// n02_stream_upload_state()'s own doc comment for why there's a byte offset
// at all) - fills both *outFrameIndex and *outByteOffset. Same return
// convention otherwise (malloc()'d buffer of just the savestate bytes,
// caller frees; NULL on any failure).
void* n02_replays_download_live_state(const char* sessionId, int* outFrameIndex, int* outByteOffset, int* outSize);

// The same for a WE Camp card's start (n02_stream_upload_checkpoint()) -
// GET /spectate/<id>/checkpoint?card=<card>; NULL while it isn't there.
void* n02_replays_download_checkpoint(const char* sessionId, int card, int* outFrameIndex, int* outByteOffset, int* outSize);
