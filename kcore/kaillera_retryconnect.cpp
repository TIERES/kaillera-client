#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "kaillera_retryconnect.h"
#include "kaillera_core.h"
#include "k_instruction.h"
#include "../common/n02_replays.h"
#include "../common/krec_reader.h"

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

bool kaillera_retryconnect_host_select(const char* session_id) {
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

	kaillera_core_debug("retry-connect: replay %s baixado em %s", session_id, dest_path);

	unsigned short len = (unsigned short)strlen(session_id);
	kaillera_retryconnect_send_select(session_id, len);

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

	kaillera_core_debug("retry-connect: replay %s (escolhido por %s) baixado em %s", session_id, fromUser, dest_path);

	kaillera_start_game();
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
	for (;;) {
		int len = 0;
		int rt = g_reader.next_record(values, size, &len);

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
		// getting *something* instead of erroring out.
		int n = min(g_last_frame_len, size);
		if (n > 0)
			memcpy(values, g_last_frame, n);
		return g_last_frame_len;
	}
}
