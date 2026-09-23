#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "kaillera_retryconnect.h"
#include "kaillera_core.h"
#include "k_instruction.h"
#include "../common/n02_replays.h"
#include "../common/krec_reader.h"

// Not declared in any shared header - kaillera_core.cpp forward-declares it
// locally the same way (see its own "int p2p_GetTime();").
int p2p_GetTime();

static bool g_active = false;
static char g_session_id[64];
static krec_reader g_reader;

// Last input frame actually served to the emulator - safety net for
// kaillera_retryconnect_modify_play_values() if the local recording runs dry
// before the host goes live (the host is expected to always pause/go live
// before that happens; this only guards the edge case where it doesn't).
static char g_last_frame[256];
static int g_last_frame_len = 0;

// One-shot mailbox for kaillera_retryconnect_poll() - the most recent
// not-yet-delivered remote RETRYCON_CONTROL. action==0 means empty.
static int g_pending_action = 0;
static int g_pending_frame = 0;

// Deferred "CONTINUANDO PARTIDA: ..." announcement (see
// kaillera_retryconnect_host_select() below) - delayed rather than sent
// immediately so it doesn't get buried under the room-join/game-start
// chatter (GAMEBEGN's own "[CORE] ..." announcement, stream-toggle notices,
// etc.) that fires around the same moment. A real GAMECHAT broadcast (see
// kaillera_retryconnect_check_pending_announce()) - the server relays it
// back to the host too, so this is the one and only line everyone in the
// room ends up seeing about the replay just picked (the technical
// "baixado em ..."/"delay is ..."/"all players ready" lines that used to
// also show up around this same moment are local-only debug noise, kept out
// of the room chat - see kaillera_core.cpp's kaillera_retryconnect_active()
// guards around those). 0 means none pending.
static char g_pending_announce[200];
static int g_pending_announce_deadline = 0;

// "DD-MM-YYYY HH:MM" (N02ReplayEntry::when, already formatted server-side)
// -> "DD/MM/YYYY HH:MM" for the user-facing announcement below.
static void FormatReplayWhen(const char* when, char* out, size_t cap) {
	size_t n = strlen(when);
	if (n >= cap) n = cap - 1;
	for (size_t i = 0; i < n; i++)
		out[i] = (when[i] == '-') ? '/' : when[i];
	out[n] = 0;
}

bool kaillera_retryconnect_active() {
	return g_active;
}

bool kaillera_retryconnect_can_control() {
	return g_active && kaillera_is_host();
}

// Downloads `session_id`'s .krec from the community server into .\records\,
// under a name that can't collide with a normal recording or another
// retry-connect download (session ids are server-assigned and unique).
static bool DownloadReplay(const char* session_id, char* dest_path_out, size_t dest_path_cap) {
	CreateDirectory("records", 0);
	_snprintf(dest_path_out, dest_path_cap, ".\\records\\retryconnect_%s.krec", session_id);
	dest_path_out[dest_path_cap - 1] = 0;
	return n02_replays_download(session_id, dest_path_out);
}

static void ResetSession() {
	g_active = false;
	g_reader.close();
	g_last_frame_len = 0;
	g_pending_action = 0;
}

bool kaillera_retryconnect_host_select(const char* session_id, const char* when) {
	char dest_path[2000];
	if (!DownloadReplay(session_id, dest_path, sizeof(dest_path))) {
		kaillera_error_callback("retry-connect: falha ao baixar o replay escolhido do servidor.");
		return false;
	}
	if (!g_reader.open_file(dest_path)) {
		kaillera_error_callback("retry-connect: falha ao abrir o replay baixado (%s).", dest_path);
		return false;
	}

	strncpy(g_session_id, session_id, sizeof(g_session_id) - 1);
	g_session_id[sizeof(g_session_id) - 1] = 0;
	g_active = true;
	g_last_frame_len = 0;
	g_pending_action = 0;

	unsigned short len = (unsigned short)strlen(session_id);
	kaillera_retryconnect_send_select(session_id, len);

	// The one line everyone in the room sees about this - see the
	// g_pending_announce comment above. Deferred a few seconds (see
	// kaillera_retryconnect_check_pending_announce()) rather than sent right
	// now, so it doesn't get buried under the room-join/game-start chatter
	// that fires around this same moment - lines up with about when the
	// peer's own auto-pause kicks in (retroarch-k3's
	// kailleraRetryConnectFrameTick()).
	char host_name[32];
	char when_fmt[24];
	kaillera_get_username(host_name, sizeof(host_name));
	FormatReplayWhen(when, when_fmt, sizeof(when_fmt));
	_snprintf(g_pending_announce, sizeof(g_pending_announce), "CONTINUANDO PARTIDA: Replay selecionado por %s: %s", host_name, when_fmt);
	g_pending_announce[sizeof(g_pending_announce) - 1] = 0;
	g_pending_announce_deadline = p2p_GetTime() + 5000;
	if (g_pending_announce_deadline == 0)
		g_pending_announce_deadline = 1; // 0 is the "none pending" sentinel

	// Converge on the normal GAMEBEGN/GAMRSRDY handshake, exactly as the
	// "Start" button would - the group replay rides on the same
	// kailleraModifyPlayValues() per-frame call that normal live play does,
	// it just sources its bytes differently (see
	// kaillera_retryconnect_modify_play_values() below).
	kaillera_start_game();
	return true;
}

void kaillera_retryconnect_select_callback(char* fromUser, char* session_id) {
	char dest_path[2000];
	if (!DownloadReplay(session_id, dest_path, sizeof(dest_path))) {
		kaillera_error_callback("retry-connect: falha ao baixar o replay que %s escolheu (%s).", fromUser, session_id);
		kaillera_retryconnect_send_nak();
		return;
	}
	if (!g_reader.open_file(dest_path)) {
		kaillera_error_callback("retry-connect: falha ao abrir o replay baixado (%s).", dest_path);
		kaillera_retryconnect_send_nak();
		return;
	}

	strncpy(g_session_id, session_id, sizeof(g_session_id) - 1);
	g_session_id[sizeof(g_session_id) - 1] = 0;
	g_active = true;
	g_last_frame_len = 0;
	g_pending_action = 0;

	// No local debug/announce line here - the host's own deferred
	// "CONTINUANDO PARTIDA: ..." (kaillera_retryconnect_host_select() above)
	// is a real GAMECHAT broadcast, so it already reaches this peer the
	// normal way a moment from now; no need to also print something local.

	// Unlike kaillera_retryconnect_host_select() above, a peer must NOT call
	// kaillera_start_game() here - that sends a "start game" request, which
	// only the room's owner is allowed to make (the server rejects anyone
	// else's with "not the owner", logging a spurious error and sending the
	// peer an "Error" game-chat message for nothing). The peer doesn't need
	// to request anything: the host's own kaillera_start_game() call (in
	// kaillera_retryconnect_host_select()) already made that request, and the
	// server broadcasts the resulting GAMEBEGN to every player in the room,
	// this peer included (see kaillera_core.cpp's "case GAMEBEGN:") - that's
	// what advances this client's own PLAYERSTAT and lets
	// kaillera_modify_play_values() reach kaillera_GameStartSequence() on its
	// own, exactly as it would for a normal (non-retry-connect) join.
}

void kaillera_retryconnect_notify_local_control(int action, int frame_index) {
	if (!kaillera_retryconnect_can_control())
		return;
	kaillera_retryconnect_send_control(action, frame_index);
	if (action == RC_ACTION_GO_LIVE) {
		// Mirrors control_callback's GO_LIVE handling below - the host
		// doesn't receive its own broadcast back (the server never echoes a
		// notification to its sender), so it has to apply this itself.
		g_active = false;
	}
}

void kaillera_retryconnect_control_callback(char* fromUser, int action, int frame_index) {
	(void)fromUser;
	// Delivered to RetroArch via kaillera_retryconnect_poll() - see there.
	g_pending_action = action;
	g_pending_frame = frame_index;

	if (action == RC_ACTION_GO_LIVE) {
		// Flips kailleraModifyPlayValues() over to the normal live path
		// starting the very next call - independent of whether/when
		// RetroArch gets around to consuming the poll() mailbox above.
		// STATE_READY does NOT trigger this: the host can pause/resend a
		// fresh savestate any number of times while still searching for the
		// right moment (see kailleraRetryConnectCaptureAndSendState(),
		// retroarch-k3 repo) - only the host's actual commit (Enter, sent as
		// a separate GO_LIVE) ends the replay-serving phase.
		g_active = false;
	}
}

bool kaillera_retryconnect_poll(int* out_action, int* out_frame_index) {
	if (g_pending_action == 0)
		return false;
	if (out_action) *out_action = g_pending_action;
	if (out_frame_index) *out_frame_index = g_pending_frame;
	g_pending_action = 0;
	return true;
}

void kaillera_retryconnect_nak_callback(char* fromUser) {
	kaillera_error_callback("retry-connect: %s nao conseguiu retomar a partir do replay escolhido.", fromUser);
	ResetSession();
}

int kaillera_retryconnect_modify_play_values(void* values, int size) {
	(void)size; // NOT the real capacity of `values` - see below.
	for (;;) {
		int len = 0;
		// `size` is the caller's per-player convention constant (e.g. 12),
		// not the physical size of `values` - `values` is RetroArch's
		// contiguous multi-player buffer (netjoy/netjoy_ex), sized for every
		// player in the room. Capping the copy at `size` (as an earlier
		// version did) silently truncated recordings with more than one
		// player's worth of bytes, leaving every player past the first with
		// stale/zero input during replay. Mirror the live path
		// (kaillera_modify_play_values()'s memcpy(values, kd+2, l), which
		// trusts the recorded length outright) by capping only at the
		// sanity bound g_last_frame already uses.
		int rt = g_reader.next_record(values, sizeof(g_last_frame), &len);

		if (rt == KREC_INPUT) {
			int n = min(len, (int)sizeof(g_last_frame));
			memcpy(g_last_frame, values, n);
			g_last_frame_len = n;
			return len;
		}

		if (rt == KREC_CHAT || rt == KREC_DROP)
			continue; // not a simulation frame - keep reading (see krec_reader.h)

		// KREC_EOF or KREC_UNKNOWN: the recording ran out before the host
		// paused/went live. Not the normal path (the host is expected to
		// stop before this point) - safety net so the emulator keeps
		// getting *something* instead of erroring out. Same reasoning as
		// above: `size` is not `values`'s real capacity, so don't cap by it.
		int n = g_last_frame_len;
		if (n > 0)
			memcpy(values, g_last_frame, n);
		return g_last_frame_len;
	}
}

void kaillera_retryconnect_check_pending_announce() {
	if (g_pending_announce_deadline != 0 && p2p_GetTime() >= g_pending_announce_deadline) {
		g_pending_announce_deadline = 0;
		kaillera_game_chat_send(g_pending_announce);
	}
}

void kaillera_retryconnect_upload_state(const void* data, int size) {
	if (!kaillera_retryconnect_can_control())
		return;

	int frame_index = g_reader.frame_index();
	if (n02_replays_upload_state(g_session_id, frame_index, data, size)) {
		// Just a fresh snapshot for the peer to reload - does NOT end the
		// replay-serving phase (see control_callback's GO_LIVE-only comment
		// above). The host may call this again after another Pause/Resume
		// cycle, any number of times, before finally committing with Enter.
		kaillera_retryconnect_send_control(RC_ACTION_STATE_READY, frame_index);
	} else {
		kaillera_error_callback("retry-connect: falha ao enviar o state save - os outros jogadores vao so pausar, sem sincronizar o frame exato.");
		kaillera_retryconnect_send_control(RC_ACTION_PAUSE, frame_index);
	}
}

int kaillera_retryconnect_download_state(void* outBuffer, int bufferCap, int* out_frame_index) {
	int frameIndex = 0, size = 0;
	void* data = n02_replays_download_state(g_session_id, &frameIndex, &size);
	if (data == NULL)
		return -1;

	int n = min(size, bufferCap);
	memcpy(outBuffer, data, n);
	free(data);

	g_reader.seek_to_frame(frameIndex);
	if (out_frame_index) *out_frame_index = frameIndex;
	return n;
}
