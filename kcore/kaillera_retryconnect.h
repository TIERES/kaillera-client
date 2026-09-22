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

// Called by the UI (the "Continuar" button's filtered replay list) once the
// host has picked a replay. Downloads it into .\records\, loads it, starts
// the normal GAMEBEGN/GAMRSRDY handshake (kaillera_start_game()) and, on
// success, broadcasts RETRYCON_SELECT so every other player in the room
// fetches the exact same file. Returns false on failure (caller should show
// an error - this function already raises one via kaillera_error_callback,
// so the caller only needs to avoid proceeding, not show its own).
bool kaillera_retryconnect_host_select(const char* session_id);

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

// Called from kaillera_core.cpp's kaillera_modify_play_values() once it's
// confirmed we're still active after draining pending instructions - serves
// the next input frame from the local recording. Same return convention as
// the emulator's normal MPV (byte length served).
int kaillera_retryconnect_modify_play_values(void* values, int size);
