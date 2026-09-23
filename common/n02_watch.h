/******************************************************************************
Live spectator playback - the reading side of n02_stream.h.

Looks up which streaming session (if any) corresponds to a Kaillera room
name, then fetches its record stream (same .krec-compatible wire format
n02_stream.h describes) from the community spectate server in the
background, so player.cpp can replay it while the host's game is still in
progress ("Watch Live").
******************************************************************************/
#pragma once

// Looks up the most recent session (preferring a still-live one over an
// already-finished one) for a Kaillera room name, via a blocking GET against
// the community spectate server's /spectate/lookup endpoint. `owner` (the
// room's "owner" column, as already shown in the lobby list) disambiguates
// two hosts who happen to have named their room the same thing - pass ""
// to skip that filter. On success copies the session id into outSessionId
// (cap bytes) and returns true. Call off the UI thread's message loop if
// possible - this blocks for the network round trip (bounded by a short
// timeout).
bool n02_watch_lookup_session(const char* room, const char* owner, char* outSessionId, int cap);

// Starts fetching session `sessionId`'s record stream on a background
// thread, from byte offset 0. Any previous watch session is stopped first.
void n02_watch_start(const char* sessionId);

// Copies up to outCap freshly-arrived bytes into outBuf, draining them from
// the background thread's prefetch buffer, and returns how many were
// copied (0 if none yet). If nothing is buffered and blockIfLive is true
// and the session hasn't been reported finished, blocks - polling in short
// sleeps - until the background thread fetches more or the session ends,
// mirroring kaillera_modify_play_values()'s own wait-for-network-data loop:
// a spectator caught up to the live edge stalls here instead of the caller
// treating the pause as end-of-stream.
int n02_watch_pull(char* outBuf, int outCap, bool blockIfLive);

// Stops the background thread. Safe to call even if nothing is active.
void n02_watch_stop();

// Restarts the background prefetch thread for the *current* session (see
// n02_watch_start() above) from a specific byte offset instead of 0 - used
// after "Ir direto para o Ao Vivo!" applies a fresh state (see
// n02_watch_download_state() below) to resume reading from wherever the host
// was when it captured that state, instead of wherever this spectator's own
// local reading had fallen behind to.
void n02_watch_restart_from_offset(int byteOffset);

// "Ir direto para o Ao Vivo!" (kaillera-client's Watch Live toolbar) -
// spectator-side half; see common/n02_stream.h for the host-side half these
// pair with. All three act on the *current* watch session (whatever
// n02_watch_start() was last called with).

// Marks a pending state request on the server - the host's own client polls
// for this and uploads a fresh state once it sees one (n02_stream.cpp).
// Returns true if the server accepted the request, not that the host has
// serviced it yet (see n02_watch_state_ready() below).
bool n02_watch_request_state();

// True once the host has serviced the most recent n02_watch_request_state()
// (the server's pending flag cleared) - call this to know when it's safe to
// n02_watch_download_state() below. Polls the server each call - the
// frontend should space these out itself (e.g. once every second or two
// while a request is outstanding), same reasoning as
// n02_stream_check_state_requested()'s own self-rate-limiting on the host
// side, just not baked in here since this side is a one-shot user action,
// not a continuous per-frame tick.
bool n02_watch_state_ready();

// Downloads the state the host uploaded into outBuffer (capacity bufferCap -
// size it from the frontend's own core_serialize_size(), matching the
// host's since a spectator runs the same core/content). *outFrameIndex gets
// the value n02_stream_upload_state() was given (locally meaningful only -
// see that function's own doc comment); *outByteOffset gets the stream
// offset to resume reading from - pass straight to
// n02_watch_restart_from_offset() above after applying the state. Returns
// the byte count written (ready for core_unserialize()), or -1 on any
// failure (including "not ready yet").
int n02_watch_download_state(void* outBuffer, int bufferCap, int* outFrameIndex, int* outByteOffset);
