#pragma once

// retry-connect - resume a dropped Kaillera-mode match from a recorded
// .krec, as a synchronized group replay the host navigates using
// RetroArch's own native Pause key (repeatable) and commits to live play
// with Enter (only valid while paused).
//
// kaillera_core.cpp/.h own the wire transport (RETRYCON_SELECT/CONTROL/NAK,
// see k_instruction.h) and just forward parsed messages to the
// kaillera_retryconnect_*_callback() functions this file implements. This
// file owns the actual behavior:
//  - downloading the host's chosen replay from the community server so
//    every player in the room ends up with byte-identical bytes;
//  - feeding kailleraModifyPlayValues() from that local recording instead of
//    the real controller while a session is active (kaillera_core.cpp's
//    kaillera_modify_play_values() calls kaillera_retryconnect_pump()/
//    kaillera_retryconnect_modify_play_values() - see kaillera_core.cpp);
//  - relaying RetroArch's native pause/resume/go-live to every other player,
//    and telling RetroArch what a remote player just did.
//
// The RetroArch-side half of this (retroarch-k3-ffw: detecting the local
// Pause/Enter presses, keeping the connection alive while natively paused,
// and calling the exported kailleraRetryConnect* functions below) is a
// separate, not-yet-implemented piece - see the project's retry-connect
// design notes. Everything in this file is ready for that side to call into.

// Called by the UI (the "Reconectar" button's filtered replay list) once the
// host has picked a replay - `when` is that entry's own N02ReplayEntry::when
// ("DD-MM-YYYY HH:MM", already formatted server-side), used only to word the
// deferred "CONTINUANDO PARTIDA: ..." room-chat announcement below (a single
// real GAMECHAT broadcast - the server relays it back to the host too, so
// everyone including the host sees the exact same wording; no separate wire
// field needed). Downloads the replay into .\records\, loads it, starts the
// normal GAMEBEGN/GAMRSRDY handshake (kaillera_start_game()) and, on success,
// broadcasts RETRYCON_SELECT so every other player in the room fetches the
// exact same file. Returns false on failure (caller should show an error -
// this function already raises one via kaillera_error_callback, so the caller
// only needs to avoid proceeding, not show its own).
bool kaillera_retryconnect_host_select(const char* session_id, const char* when);

// True from a successful select (host or peer) until a RETRYCON_CONTROL
// GO_LIVE or RETRYCON_NAK ends the attempt. Gates
// kaillera_modify_play_values()'s retry-connect fast path, and also makes
// kailleraIsPlaybackMode() report true (see kailleraclient.cpp) so RetroArch
// allows fast-forward during the replay, same as it already does for actual
// Playback mode.
bool kaillera_retryconnect_active();

// True only for the room's host while a session is active - gates whether
// RetroArch should let the local Pause/Enter keys do anything at all during
// retry-connect (a peer's local presses must not affect anyone; only the
// host navigates). RetroArch should call this (via a new export) before
// acting on either key.
bool kaillera_retryconnect_can_control();

// Called (via a new export) when RetroArch detects the LOCAL user (already
// confirmed to be the host - see kaillera_retryconnect_can_control() above)
// pressed native Pause/Resume, or Enter to go live. `frame_index` is
// RetroArch's own authoritative frame counter (current_core_frame in
// kaillera.c) at the moment of the action - relayed to every other player
// unchanged via RETRYCON_CONTROL. No-op if kaillera_retryconnect_can_control()
// is false (defense in depth - RetroArch is expected to have already gated
// this itself).
void kaillera_retryconnect_notify_local_control(int action, int frame_index);

// Called (via a new export) once per RetroArch tick - including while
// natively paused, which is why this needs its own poll rather than reusing
// kailleraModifyPlayValues() (RetroArch stops calling that while paused).
// Returns true and fills *out_action/*out_frame_index exactly once per
// remote RETRYCON_CONTROL received (PAUSE/RESUME/GO_LIVE - RetroArch should
// mirror it: call the matching command_event(), and for GO_LIVE also stop
// expecting replay-sourced input - kaillera_retryconnect_active() already
// flips to false for that case). Returns false when there's nothing new.
bool kaillera_retryconnect_poll(int* out_action, int* out_frame_index);

// Called once per kaillera_retryconnect_pump() (kaillera_core.cpp) - fires
// the host's deferred "CONTINUANDO PARTIDA: ..." game-chat announcement
// (queued by kaillera_retryconnect_host_select()) once its few-second delay
// has elapsed. No-op with nothing pending (the overwhelmingly common case,
// since this only ever has something queued for a few seconds right after
// "Reconectar").
void kaillera_retryconnect_check_pending_announce();

// Called from kaillera_core.cpp's kaillera_modify_play_values() once it's
// confirmed we're still active after draining pending instructions - serves
// the next input frame from the local recording. Same return convention as
// the emulator's normal MPV (byte length served).
int kaillera_retryconnect_modify_play_values(void* values, int size);

// Host-only fast-forward handoff. Fast-forwarding a group replay is host-only
// and purely local (RetroArch's own hotkey handling, not this file) precisely
// because different machines/cores aren't guaranteed to reach the same frame
// at the same real-world time - so the moment the host stops, it calls this
// (via a new export) with a full core savestate (RetroArch's core_serialize()
// output) instead of trying to get everyone to reproduce the same frame by
// replaying it themselves. Uploads `data`/`size` to the community server and,
// on success, broadcasts RC_ACTION_STATE_READY(frame_index) - frame_index is
// this reader's own current position, meaningful across machines because
// kaillera_retryconnect_host_select()/select_callback() already guarantee
// every client is reading the exact same file. Falls back to a plain
// RC_ACTION_PAUSE broadcast if the upload itself fails, so peers at least
// stop (even though no longer frame-accurate with the host). No-op if not
// host (defense in depth - RetroArch is expected to have already gated this
// via kaillera_retryconnect_can_control()).
void kaillera_retryconnect_upload_state(const void* data, int size);

// Companion to kaillera_retryconnect_upload_state() above, called (via a new
// export) once RetroArch sees a remote RC_ACTION_STATE_READY (from
// kaillera_retryconnect_poll()). Downloads the state into outBuffer (capacity
// bufferCap - the caller must size this from its own core_serialize_size(),
// which has to match the host's since every client runs the same
// core/content) and, on success, re-anchors this reader's position to the
// exact frame the state was taken at (see krec_reader::seek_to_frame()) so
// future reads/pauses/go-lives from this same session stay aligned -
// regardless of whether this reader was already ahead of or behind that
// frame. Fills *out_frame_index (may be NULL) for the caller's own
// informational use; the repositioning above happens either way. Returns the
// number of bytes written to outBuffer (hand straight to core_unserialize()),
// or -1 on any failure (network, no state uploaded yet, buffer too small).
int kaillera_retryconnect_download_state(void* outBuffer, int bufferCap, int* out_frame_index);

// Host-only local rewind support (toolbar "Rebobinar" button, retroarch-k3).
// Unlike kaillera_retryconnect_download_state() above, these three never
// touch the network by themselves - the frontend is expected to (1) restore
// its own core state from a locally-kept checkpoint (a core_serialize() blob
// it captured earlier at some prior frame - this file has no opinion on how
// those are kept, that's the frontend's own checkpoint ring, mirroring the
// one already built for solo "Reproducao de Replay"), (2) call
// kaillera_retryconnect_seek_local() to re-anchor this reader to that exact
// frame, then (3) call the existing kaillera_retryconnect_upload_state() with
// that same checkpoint's bytes - which reads g_reader's now-updated position
// as its frame_index and broadcasts RC_ACTION_STATE_READY exactly as a normal
// Pause would, so every peer converges via the SAME already-working
// ApplyRetryConnectStateReady() path with no peer-side changes needed.

// "Sem M. Card" of the replay being resumed (1 = no memory card, 0 = with -
// also for replays without the marker, see krec_reader.h): every player
// resumes it the way it was played, whatever the room's checkbox says.
int kaillera_retryconnect_no_memcard();

// Current position in the replay - for a caller-side progress bar and to
// compute rewind targets relative to "right now". -1 if no session active.
int kaillera_retryconnect_get_frame_index();

// Total input-frame count of the replay being resumed - for a caller-side
// progress bar. Cached once when the file is opened (kaillera_retryconnect_
// host_select()/select_callback()), not recomputed per call. -1 if no
// session active.
int kaillera_retryconnect_get_total_frames();

// Repositions g_reader to `frame_index` with no network I/O - see the big
// comment above. Host-only (kaillera_retryconnect_can_control()); a peer's
// own view is always authoritatively overwritten by the host's next
// broadcast anyway, so peers have no legitimate reason to call this.
void kaillera_retryconnect_seek_local(int frame_index);
