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
