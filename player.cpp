#include "player.h"
#include "kailleraclient.h"
#include "resource.h"
#include "uihlp.h"
#include <time.h>
#include <shellapi.h>
#include "errr.h"
#include "common/nSettings.h"
#include "common/n02_watch.h"
#include "common/n02_replays.h"
#include "common/krec_reader.h"

static void UpdateModeRadioButtons(HWND hDlg){
	int mode = get_active_mode_index();
	if (mode < 0 || mode > 2)
		mode = 1;
	CheckRadioButton(hDlg, RB_MODE_P2P, RB_MODE_PLAYBACK, RB_MODE_P2P + mode);
}

bool player_playing = false;
static bool player_was_dropped[16] = {};
static bool player_watch_mode = false;
static char g_pending_watch_session[64] = { 0 };
static char g_pending_watch_room[128] = { 0 };
static char g_watch_player_names[4][32] = {};
void (*player_watch_ended_callback)() = NULL;

// "Replays Online" checkbox state - when checked, the Records list shows
// N02ReplayEntry entries fetched from the community server instead of local
// files (see RecordsList_PopulateOnline()), and Play/Delete act on those
// instead (download-then-play, download-only).
static bool g_online_mode = false;
static N02ReplayEntry g_online_entries[N02_REPLAYS_MAX_ENTRIES];
static int g_online_count = 0;

class PlayBackBufferC {
public:
	char * buffer;
	char * ptr;
	char * end;
    
	void load_bytes(void* arg_0, unsigned int arg_4) {
		if (ptr + 10 < end) {
			int p = min(arg_4, (unsigned int)(end - ptr));
			memcpy(arg_0, ptr, p);
			ptr += p;
		}
	}
	void load_str(char* arg_0, unsigned int arg_4) {
		arg_4 = min(arg_4, (unsigned int)strlen(ptr) + 1);
		arg_4 = min(arg_4, (unsigned int)(end - ptr + 1));
		load_bytes(arg_0, arg_4);
		arg_0[arg_4] = 0x00;
	}
	int load_int(){
        int x;
        load_bytes(&x,4);
        return x;
    }
    unsigned char load_char(){
        unsigned char x;
        load_bytes(&x,1);
        return x;
    }
    unsigned short load_short(){
        unsigned short x;
        load_bytes(&x,2);
        return x;
    }
	
} PlayBackBuffer;

//==============================================

extern HWND RecordsListDlg; // defined below; used by player_watch_begin()'s error MessageBox

// --- Watch Live rewind support (bounded retention window) ---
// Kaillera controller data is tiny (a few bytes/frame/player), so retaining
// a generous window of already-consumed bytes - instead of discarding it the
// instant it's consumed, as PlayBackBuffer_Append() did before - costs very
// little memory even over a long spectate session, and is what lets the
// on-screen toolbar's Rewind jump backward during Watch Live the same way it
// already does for static Playback (see player_watch_seek_to_frame() below;
// mirrors krec_reader::seek_to_frame()'s own technique of resetting the read
// position and re-walking forward). Bounded by count+frame-gap rather than
// wall-clock time - approximates "last ~10-15 minutes" for a typical match
// without needing a clock.
#define WATCH_RETAIN_MAX_BYTES (16 * 1024 * 1024)
#define WATCH_SNAPSHOT_COUNT 100
#define WATCH_SNAPSHOT_MIN_FRAME_GAP 600 // ~10s at 60fps; 100 slots -> ~16-17 min of history

typedef struct {
	int frame_index; // g_watch_frames_consumed at the time this was recorded
	int offset;       // byte offset from PlayBackBuffer.buffer
} WatchSnapshot;

static WatchSnapshot g_watch_snapshots[WATCH_SNAPSHOT_COUNT];
static int g_watch_snapshot_count = 0;
static int g_watch_frames_consumed = 0;

static void WatchSnapshotReset() {
	g_watch_snapshot_count = 0;
	g_watch_frames_consumed = 0;
}

// Called after appending a freshly-pulled chunk - cheap, and frequent enough
// (once per refill) for reasonably fine-grained rewind without any extra
// timer/bookkeeping elsewhere. No-ops unless enough frames have elapsed
// since the last recorded snapshot (see WATCH_SNAPSHOT_MIN_FRAME_GAP).
static void WatchSnapshotRecord() {
	if (g_watch_snapshot_count > 0 &&
		g_watch_frames_consumed - g_watch_snapshots[g_watch_snapshot_count - 1].frame_index < WATCH_SNAPSHOT_MIN_FRAME_GAP)
		return;

	if (g_watch_snapshot_count == WATCH_SNAPSHOT_COUNT) {
		memmove(&g_watch_snapshots[0], &g_watch_snapshots[1], sizeof(WatchSnapshot) * (WATCH_SNAPSHOT_COUNT - 1));
		g_watch_snapshot_count--;
	}
	WatchSnapshot* slot = &g_watch_snapshots[g_watch_snapshot_count++];
	slot->frame_index = g_watch_frames_consumed;
	slot->offset = (int)(PlayBackBuffer.ptr - PlayBackBuffer.buffer);
}

// Appends freshly-pulled bytes for watch mode's ever-growing playback - this
// is how watch mode keeps the same load_bytes()/load_short()/load_str()
// parsing player_MPV() already uses for static .krec playback working
// unchanged against a buffer that keeps growing during a live game.
//
// Retains up to WATCH_RETAIN_MAX_BYTES of already-consumed history (before
// ptr) in addition to the always-tiny unconsumed tail, so
// player_watch_seek_to_frame() below has somewhere to rewind into - only
// bytes older than that window get permanently dropped.
static void PlayBackBuffer_Append(const char* data, int len) {
	if (len <= 0) return;
	int unconsumed = (int)(PlayBackBuffer.end - PlayBackBuffer.ptr);
	if (unconsumed < 0) unconsumed = 0;

	int consumed_history = (int)(PlayBackBuffer.ptr - PlayBackBuffer.buffer);
	if (consumed_history < 0) consumed_history = 0;
	int retain = min(consumed_history, WATCH_RETAIN_MAX_BYTES);
	int dropped = consumed_history - retain; // permanently discarded this call, if any

	int total = retain + unconsumed + len;
	char* nb = (char*)malloc(total);
	if (nb == NULL) return; // OOM: chunk dropped, next refill attempt tries again
	if (retain > 0)
		memcpy(nb, PlayBackBuffer.ptr - retain, retain);
	if (unconsumed > 0)
		memcpy(nb + retain, PlayBackBuffer.ptr, unconsumed);
	memcpy(nb + retain + unconsumed, data, len);
	free(PlayBackBuffer.buffer);
	PlayBackBuffer.buffer = nb;
	PlayBackBuffer.ptr = nb + retain;
	PlayBackBuffer.end = nb + total;

	// Snapshot offsets are relative to `buffer` - shift them to match, and
	// drop any whose target byte just got permanently discarded.
	if (dropped > 0) {
		int w = 0;
		for (int i = 0; i < g_watch_snapshot_count; i++) {
			if (g_watch_snapshots[i].offset >= dropped) {
				g_watch_snapshots[i].offset -= dropped;
				if (w != i) g_watch_snapshots[w] = g_watch_snapshots[i];
				w++;
			}
		}
		g_watch_snapshot_count = w;
	}

	WatchSnapshotRecord();
}

// Called from player_MPV() when watch mode's buffer has run dry. Blocks (in
// short sleeps, via n02_watch_pull's blockIfLive) as long as the session is
// still live and nothing new has arrived - a spectator caught up to the
// live edge stalls here instead of player_MPV ending playback, same as
// kaillera_modify_play_values() stalls waiting for the next network frame.
static void PlayBackBuffer_WatchRefill() {
	char chunk[64 * 1024];
	int n = n02_watch_pull(chunk, sizeof(chunk), true);
	if (n > 0)
		PlayBackBuffer_Append(chunk, n);
}

void player_request_watch(const char* sessionId, const char* roomName) {
	strncpy(g_pending_watch_session, (sessionId != NULL) ? sessionId : "", sizeof(g_pending_watch_session) - 1);
	g_pending_watch_session[sizeof(g_pending_watch_session) - 1] = 0;
	strncpy(g_pending_watch_room, (roomName != NULL) ? roomName : "", sizeof(g_pending_watch_room) - 1);
	g_pending_watch_room[sizeof(g_pending_watch_room) - 1] = 0;
}

// Starts spectating: fetches session `sessionId`'s stream, waits for its
// 400-byte KRC1 header (always the first bytes of any session - see
// n02_stream.h), then starts the same KSSDFA_START_GAME sequence player_play()
// uses for a static .krec file. Unlike player_play(), there's no local file
// and no "emulator mismatch" prompt - the spectator isn't required to run
// the same emulator build as the host. Public (see player.h) - callable
// directly from any active module, not just from player_GUI() picking up a
// player_request_watch() request.
bool player_watch_begin(const char* sessionId, const char* roomName) {
	n02_TRACE();
	if (player_playing) player_EndGame();

	if (PlayBackBuffer.buffer != NULL) {
		free(PlayBackBuffer.buffer);
		PlayBackBuffer.buffer = NULL;
	}

	n02_watch_start(sessionId);

	char header[400];
	int have = 0;
	DWORD startTick = GetTickCount();
	while (have < 400 && GetTickCount() - startTick < 5000) {
		int n = n02_watch_pull(header + have, 400 - have, false);
		if (n > 0)
			have += n;
		else
			Sleep(20);
	}
	if (have < 400) {
		n02_watch_stop();
		char msg[256];
		wsprintf(msg, "Timed out waiting for the \"%s\" stream to start.", roomName);
		MessageBox(RecordsListDlg, msg, "Error", MB_OK | MB_ICONSTOP);
		return false;
	}

	PlayBackBuffer.buffer = (char*)malloc(400);
	memcpy(PlayBackBuffer.buffer, header, 400);
	PlayBackBuffer.end = PlayBackBuffer.buffer + 400;

	PlayBackBuffer.ptr = PlayBackBuffer.buffer + 132;
	PlayBackBuffer.load_str(GAME, 128);

	PlayBackBuffer.ptr = PlayBackBuffer.buffer + 264;
	PlayBackBuffer.load_int(); // host's own playerno - not meaningful to a spectator
	numplayers = PlayBackBuffer.load_int();
	// playerno=0 isn't a valid Kaillera player slot (the base protocol has no
	// "spectator" concept), and passing it to gameCallback() made at least
	// one emulator frontend silently refuse to start - use 1 instead, and
	// player_MPV()'s drop handling below skips the "self dropped" check in
	// watch mode so a real player 1 dropping doesn't end the spectator's feed.
	playerno = 1;

	// recording_player_names (see kailleraclient.cpp's recording writer):
	// 4x32 bytes right after numplayers, i.e. header offset 272.
	memset(g_watch_player_names, 0, sizeof(g_watch_player_names));
	PlayBackBuffer.ptr = PlayBackBuffer.buffer + 272;
	for (int i = 0; i < 4; i++)
		PlayBackBuffer.load_str(g_watch_player_names[i], sizeof(g_watch_player_names[i]));

	PlayBackBuffer.ptr = PlayBackBuffer.buffer + 400;

	WatchSnapshotReset();
	WatchSnapshotRecord(); // frame-0 baseline, so rewinding works even before the first refill

	player_watch_mode = true;
	player_playing = true;
	memset(player_was_dropped, 0, sizeof(player_was_dropped));

	KSSDFA.input = KSSDFA_START_GAME;
	n02_TRACE();
	return true;
}

bool player_is_watching() {
	return player_watch_mode;
}

void player_watch_get_player_names(char out[4][32]) {
	memcpy(out, g_watch_player_names, sizeof(g_watch_player_names));
}

// "Ir direto para o Ao Vivo!" - called right after the frontend applies a
// state downloaded via n02_watch_download_state() (core_unserialize()).
// Unlike WatchSeekToFrame() above (which rewinds *within* what's already
// retained locally), this jumps *forward* past whatever this spectator had
// buffered - the downloaded state already reflects everything up to
// byteOffset in the host's stream, so anything we had buffered before that
// point is now stale and would double-apply input if fed to the emulator.
// Discards the buffer entirely and restarts fetching from byteOffset (the
// host's own stream position when it captured the state - see
// n02_stream_upload_state()'s doc comment), rather than trying to reconcile
// with whatever this spectator's own reading had fallen behind to.
void player_watch_jump_to_live(int frameIndex, int byteOffset) {
	if (!player_playing || !player_watch_mode)
		return;

	if (PlayBackBuffer.buffer != NULL) {
		free(PlayBackBuffer.buffer);
		PlayBackBuffer.buffer = NULL;
	}
	PlayBackBuffer.ptr = NULL;
	PlayBackBuffer.end = NULL;

	WatchSnapshotReset(); // also zeroes g_watch_frames_consumed - set it after
	g_watch_frames_consumed = frameIndex;

	n02_watch_restart_from_offset(byteOffset);
}

//..............................................

///////////////////////////////////////////////////////////////////////////////



#define MAX_RECORDS 512
nLVw RecordsListDlg_list;
HWND RecordsListDlg;
char record_filenames[MAX_RECORDS][260];
// Static local-file playback (retry-connect Fase 1): reads via the shared
// krec_reader helper instead of hand-parsing PlayBackBuffer directly. Watch
// mode (player_watch_begin(), further below) still uses the legacy
// PlayBackBufferC global unchanged - only this local-file path was migrated.
static krec_reader g_playback_reader;
static int g_playback_total_frames = -1; // cached at open time - see player_get_total_frames()

void player_play(char * fn){
	n02_TRACE();
	//char * fn = BrowseFile(0);
	if(fn== 0)
		return;

	if (!g_playback_reader.open_file(fn)) {
		MessageBox(RecordsListDlg, "File too short", "Error", MB_OK | MB_ICONSTOP);
		return;
	}

	if (strcmp(APP, g_playback_reader.appName) != 0) {
		char wdr[2000];
		wsprintf(wdr, "Application name mismatch.\nExpected \"%s\" but recieved \"%s\".\nUsing a different emulator for playback may cause things to behave in an unexpected manner.\nDo you want to continue?", g_playback_reader.appName, APP);
		if (MessageBox(RecordsListDlg, wdr, "Error", MB_YESNO | MB_ICONEXCLAMATION) != IDYES) {
			g_playback_reader.close();
			return;
		}
	}

	strcpy(GAME, g_playback_reader.gameName);
	playerno = g_playback_reader.playerno;
	numplayers = g_playback_reader.numplayers;
	g_playback_total_frames = g_playback_reader.count_total_frames();

	player_playing = true;
	memset(player_was_dropped, 0, sizeof(player_was_dropped));

	KSSDFA.input = KSSDFA_START_GAME;

	n02_TRACE();
}
void RecordsList_PlaySelected(){
	n02_TRACE();
	if (player_playing) return;
	int s = RecordsListDlg_list.SelectedRow();
	if (s < 0 || s >= RecordsListDlg_list.RowsCount()) return;
	int idx = (int)RecordsListDlg_list.RowNo(s);

	if (g_online_mode) {
		if (idx < 0 || idx >= g_online_count) return;
		CreateDirectory(".\\records", 0);
		char destPath[2000];
		wsprintf(destPath, ".\\records\\%s", g_online_entries[idx].download_name);
		if (!n02_replays_download(g_online_entries[idx].session_id, destPath)) {
			MessageBox(RecordsListDlg, "Failed to download the replay from the server.", "Error", MB_OK | MB_ICONSTOP);
			return;
		}
		player_play(destPath);
		return;
	}

	if (idx >= 0 && idx < MAX_RECORDS){
		char filename[2000];
		wsprintf(filename, ".\\records\\%s", record_filenames[idx]);
		player_play(filename);
	}
}

void RecordsList_Populate_fn(char * fn, int i) {
	n02_TRACE();
	if (i < MAX_RECORDS) {
		strncpy(record_filenames[i], fn, 259);
		record_filenames[i][259] = 0;
	}
	char filename[2000];
	wsprintf(filename, ".\\records\\%s", fn);
	
	CreateDirectory(".\\records", 0);

	HANDLE in = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (in == INVALID_HANDLE_VALUE) {
		RecordsListDlg_list.AddRow(fn, i);
		RecordsListDlg_list.FillRow("Error", 1, i);
		return;
	}
	DWORD len = SetFilePointer(in, 0, NULL, FILE_END);
	SetFilePointer(in, 0, NULL, FILE_BEGIN);
	if (len < 300) {
		CloseHandle(in);
		RecordsListDlg_list.AddRow(fn, i);
		RecordsListDlg_list.FillRow("File too short", 1, i);
		return;
	}
	char* filebuf = (char*)malloc(len + 1);
	if (!filebuf) { CloseHandle(in); return; }
	DWORD bytesRead;
	ReadFile(in, filebuf, len, &bytesRead, NULL);
	CloseHandle(in);
	PlayBackBuffer.buffer = filebuf;
	PlayBackBuffer.ptr = filebuf;
	PlayBackBuffer.end = filebuf + len;
	char VER[5];
	memcpy(VER, filebuf, 4);
	VER[4] = 0;
	bool isKRC1 = (strcmp(VER, "KRC1") == 0);
	if (strcmp(VER, "KRC0") != 0 && !isKRC1) {
		free(filebuf);
		PlayBackBuffer.buffer = PlayBackBuffer.ptr = PlayBackBuffer.end = NULL;
		return;
	}
	DWORD headerSize = isKRC1 ? 400 : 272;
	if (len < headerSize) {
		free(filebuf);
		PlayBackBuffer.buffer = PlayBackBuffer.ptr = PlayBackBuffer.end = NULL;
		return;
	}
	PlayBackBuffer.ptr = PlayBackBuffer.buffer + 4;
	char APPC[128];
	PlayBackBuffer.load_str(APPC, 128);
	PlayBackBuffer.ptr = PlayBackBuffer.buffer + 132;	
	PlayBackBuffer.load_str(GAME,128);
	PlayBackBuffer.ptr = PlayBackBuffer.buffer + 260;
	time_t timee = PlayBackBuffer.load_int();
	playerno = PlayBackBuffer.load_int();
	numplayers = PlayBackBuffer.load_int();
	// Col 0: Date - KRC1 uses header timestamp, KRC0 parses filename with header fallback
	{
		bool parsed = false;
		if (!isKRC1 && strlen(fn) > 13) {
			bool allDigits = true;
			for (int d = 0; d < 12; d++) {
				if (!isdigit(fn[d])) { allDigits = false; break; }
			}
			if (allDigits && fn[12] == '-') {
				sprintf(filename, "%c%c/%c%c/%c%c %c%c:%c%c",
					fn[0], fn[1], fn[2], fn[3], fn[4], fn[5],
					fn[6], fn[7], fn[8], fn[9]);
				parsed = true;
			}
		}
		if (!parsed) {
			tm * ecx = localtime(&timee);
			if (ecx) {
				sprintf(filename, "%02d/%02d/%02d %02d:%02d", ecx->tm_year % 100, ecx->tm_mon + 1, ecx->tm_mday, ecx->tm_hour, ecx->tm_min);
			} else {
				strcpy(filename, "?");
			}
		}
	}
	RecordsListDlg_list.AddRow(filename, i);

	// Col 1: Players - read from header (KRC1) or parse filename (KRC0)
	{
		char players[256];
		players[0] = 0;
		if (isKRC1) {
			// Read player names from header offsets 272-399 (4 x 32 bytes)
			for (int p = 0; p < numplayers && p < 4; p++) {
				char name[33];
				memcpy(name, filebuf + 272 + p * 32, 32);
				name[32] = 0;
				if (name[0] == 0) continue;
				if (players[0] != 0) strcat(players, ", ");
				strncat(players, name, 255 - strlen(players));
			}
			if (players[0] == 0) strcpy(players, "?");
		} else {
			int nameStart = 0;
			if (strlen(fn) > 13) {
				bool allDigits = true;
				for (int d = 0; d < 12; d++) {
					if (!isdigit(fn[d])) { allDigits = false; break; }
				}
				if (allDigits && fn[12] == '-') nameStart = 13;
			}
			if (nameStart > 0) {
				const char* p = fn + nameStart;
				const char* ext = strstr(fn, ".krec");
				const char* lastDash = NULL;
				for (const char* scan = p; scan < (ext ? ext : fn + strlen(fn)); scan++) {
					if (*scan == '-') lastDash = scan;
				}
				if (lastDash && lastDash > p) {
					int plen = (int)(lastDash - p);
					if (plen > 255) plen = 255;
					strncpy(players, p, plen);
					players[plen] = 0;
					char display[256];
					display[0] = 0;
					char* tok = strtok(players, "-");
					while (tok) {
						if (display[0] != 0) strcat(display, ", ");
						strcat(display, tok);
						tok = strtok(NULL, "-");
					}
					strcpy(players, display);
				} else {
					strcpy(players, "?");
				}
			} else {
				strcpy(players, "?");
			}
		}
		RecordsListDlg_list.FillRow(players, 1, i);
	}

	// Col 2: Game name from header
	RecordsListDlg_list.FillRow(GAME, 2, i);

	// Col 3: Duration - scan records and count input frames
	{
		int frames = 0;
		char* scan = filebuf + headerSize; // skip header
		char* scanEnd = filebuf + len;
		while (scan + 1 < scanEnd) {
			unsigned char type = (unsigned char)*scan++;
			if (type == 0x12) {
				if (scan + 2 > scanEnd) break;
				unsigned short rlen = *(unsigned short*)scan;
				scan += 2;
				if (rlen > 0) {
					if (scan + rlen > scanEnd) break;
					scan += rlen;
				}
				frames++;
			} else if (type == 0x14) { // drop: null-terminated nick + 4 bytes
				while (scan < scanEnd && *scan != 0) scan++;
				if (scan < scanEnd) scan++; // skip null
				scan += 4; // player number
			} else if (type == 0x08) { // chat: two null-terminated strings
				while (scan < scanEnd && *scan != 0) scan++;
				if (scan < scanEnd) scan++; // skip null
				while (scan < scanEnd && *scan != 0) scan++;
				if (scan < scanEnd) scan++; // skip null
			} else {
				break; // unknown record type
			}
		}
		int totalSec = frames / 60;
		int mins = totalSec / 60;
		int secs = totalSec % 60;
		sprintf(filename, "%d:%02d", mins, secs);
		RecordsListDlg_list.FillRow(filename, 3, i);
	}

	// Col 4: Size (file size)
	if (len <= 1024) {
		wsprintf(filename, "%i B", len);
	}
	else {
		len /= 1024;
		if (len < 1000) {
			sprintf(filename, "%i kB", len);
		}
		else {
			int mb = len / 1000;
			int frc = (len % 1000) / 100;
			sprintf(filename, "%i.%i MB", mb, frc);
		}
	}
	RecordsListDlg_list.FillRow(filename, 4, i);

	// Col 5: Filename
	RecordsListDlg_list.FillRow(fn, 5, i);
	free(filebuf);
	// RecordsList_Populate() runs this per file purely to fill in the list's
	// columns, reusing the shared PlayBackBuffer as scratch space via
	// load_str()/load_int() above. Leaving PlayBackBuffer.buffer pointing at
	// this now-freed block made player_watch_begin()'s "free the previous
	// buffer" check (it runs right after RecordsList_Populate() in
	// RecordsListDlgProc's WM_INITDIALOG) double-free it - crashing as soon
	// as "Acompanhar ao vivo!" was used with any local .krec already listed.
	PlayBackBuffer.buffer = PlayBackBuffer.ptr = PlayBackBuffer.end = NULL;
	n02_TRACE();
}

static int __cdecl fn_compare_desc(const void *a, const void *b) {
	// Reverse strcmp so newest (highest timestamp) comes first
	return strcmp((const char*)b, (const char*)a);
}

void RecordsList_Populate(){
	n02_TRACE();
	RecordsListDlg_list.DeleteAllRows();
	CreateDirectory(".\\records", 0);

	// Collect filenames
	char collected[MAX_RECORDS][260];
	int count = 0;

	WIN32_FIND_DATA FindFileData;
	HANDLE hFind = FindFirstFile(".\\records\\*", &FindFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && count < MAX_RECORDS) {
				strncpy(collected[count], FindFileData.cFileName, 259);
				collected[count][259] = 0;
				count++;
			}
		} while (FindNextFile(hFind, &FindFileData) != 0);
		FindClose(hFind);
	}

	// Sort descending (newest first)
	qsort(collected, count, sizeof(collected[0]), fn_compare_desc);

	for (int i = 0; i < count; i++) {
		RecordsList_Populate_fn(collected[i], i);
	}
}
void RecordsList_DeleteSelected(){
	int s = RecordsListDlg_list.SelectedRow();
	if (s >= 0 && s < RecordsListDlg_list.RowsCount()){
		int idx = (int)RecordsListDlg_list.RowNo(s);
		if (idx >= 0 && idx < MAX_RECORDS){
			char filename[2000];
			wsprintf(filename, ".\\records\\%s", record_filenames[idx]);
			DeleteFile(filename);
			RecordsList_Populate();
		}
	}
}

// BTN_DELETE's online-mode counterpart: downloads the selected online replay
// into .\records\ without starting playback (mirrors RecordsList_DeleteSelected()'s
// selection handling, but downloads instead of deleting since there's nothing
// local to remove).
void RecordsList_DownloadSelected(){
	int s = RecordsListDlg_list.SelectedRow();
	if (s < 0 || s >= RecordsListDlg_list.RowsCount()) return;
	int idx = (int)RecordsListDlg_list.RowNo(s);
	if (idx < 0 || idx >= g_online_count) return;

	CreateDirectory(".\\records", 0);
	char destPath[2000];
	wsprintf(destPath, ".\\records\\%s", g_online_entries[idx].download_name);
	if (n02_replays_download(g_online_entries[idx].session_id, destPath)) {
		MessageBox(RecordsListDlg, "Replay downloaded to the records folder.", "Download", MB_OK | MB_ICONINFORMATION);
	} else {
		MessageBox(RecordsListDlg, "Failed to download the replay from the server.", "Error", MB_OK | MB_ICONSTOP);
	}
}

static void FormatReplayDuration(int totalSeconds, char* out) {
	int mins = totalSeconds / 60;
	int secs = totalSeconds % 60;
	sprintf(out, "%d:%02d", mins, secs);
}

static void FormatReplaySize(int len, char* out) {
	if (len <= 1024) {
		wsprintf(out, "%i B", len);
	} else {
		len /= 1024;
		if (len < 1000) {
			sprintf(out, "%i kB", len);
		} else {
			int mb = len / 1000;
			int frc = (len % 1000) / 100;
			sprintf(out, "%i.%i MB", mb, frc);
		}
	}
}

// Fills the Records list from the community server's replay list instead of
// the local .\records\ folder - see the CHK_ONLINE handler in
// RecordsListDlgProc(). Uses the same 6 columns as the local list
// (RecordsList_Populate_fn()) so no layout/column code needs to differ.
void RecordsList_PopulateOnline(){
	RecordsListDlg_list.DeleteAllRows();
	g_online_count = n02_replays_fetch_list(g_online_entries, N02_REPLAYS_MAX_ENTRIES);
	if (g_online_count == 0) {
		MessageBox(RecordsListDlg, "Could not fetch the replay list from the server.", "Replays Online", MB_OK | MB_ICONEXCLAMATION);
		return;
	}
	for (int i = 0; i < g_online_count; i++) {
		N02ReplayEntry* e = &g_online_entries[i];
		char buf[64];
		RecordsListDlg_list.AddRow(e->when, i);
		RecordsListDlg_list.FillRow(e->player_names[0] ? e->player_names : (char*)"?", 1, i);
		RecordsListDlg_list.FillRow(e->game_name, 2, i);
		FormatReplayDuration(e->duration_seconds, buf);
		RecordsListDlg_list.FillRow(buf, 3, i);
		FormatReplaySize(e->size_bytes, buf);
		RecordsListDlg_list.FillRow(buf, 4, i);
		RecordsListDlg_list.FillRow(e->download_name, 5, i);
	}
}

#define PB_NUM_COLS 6
#define IDM_COL_TOGGLE 40100

static const char* colKeys[PB_NUM_COLS] = { "PBColDate", "PBColPlayers", "PBColGame", "PBColDuration", "PBColSize", "PBColFilename" };
static const char* colVisKeys[PB_NUM_COLS] = { "PBVisDate", "PBVisPlayers", "PBVisGame", "PBVisDuration", "PBVisSize", "PBVisFilename" };
static const int colDefaults[PB_NUM_COLS] = { 80, 160, 200, 60, 60, 150 };
static const char* colNames[PB_NUM_COLS] = { "Date", "Players", "Game", "Duration", "Size", "Filename" };
static bool col_visible[PB_NUM_COLS];
static int col_saved_width[PB_NUM_COLS];

static int sort_column = -1;
static bool sort_ascending = true;

static int CALLBACK RecordsList_CompareItems(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
	char text1[256], text2[256];
	ListView_GetItemText(RecordsListDlg_list.handle, (int)lParam1, sort_column, text1, sizeof(text1));
	ListView_GetItemText(RecordsListDlg_list.handle, (int)lParam2, sort_column, text2, sizeof(text2));
	int result = strcmp(text1, text2);
	return sort_ascending ? result : -result;
}

static void SavePlaybackState(HWND hDlg) {
	for (int i = 0; i < PB_NUM_COLS; i++) {
		int w = ListView_GetColumnWidth(RecordsListDlg_list.handle, i);
		if (w > 0) {
			nSettings::set_int((char*)colKeys[i], w);
			col_saved_width[i] = w;
		}
		nSettings::set_int((char*)colVisKeys[i], col_visible[i] ? 1 : 0);
	}
	RECT rc;
	GetWindowRect(hDlg, &rc);
	nSettings::set_int((char*)"PBWinW", rc.right - rc.left);
	nSettings::set_int((char*)"PBWinH", rc.bottom - rc.top);
}

static void ShowColumnContextMenu(HWND hDlg) {
	HMENU hMenu = CreatePopupMenu();
	for (int i = 0; i < PB_NUM_COLS; i++) {
		UINT flags = MF_STRING;
		if (col_visible[i]) flags |= MF_CHECKED;
		AppendMenu(hMenu, flags, IDM_COL_TOGGLE + i, colNames[i]);
	}
	POINT pt;
	GetCursorPos(&pt);
	int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hDlg, NULL);
	DestroyMenu(hMenu);
	if (cmd >= IDM_COL_TOGGLE && cmd < IDM_COL_TOGGLE + PB_NUM_COLS) {
		int col = cmd - IDM_COL_TOGGLE;
		col_visible[col] = !col_visible[col];
		if (col_visible[col]) {
			int w = col_saved_width[col];
			if (w < 20) w = colDefaults[col];
			ListView_SetColumnWidth(RecordsListDlg_list.handle, col, w);
		} else {
			col_saved_width[col] = ListView_GetColumnWidth(RecordsListDlg_list.handle, col);
			if (col_saved_width[col] < 20) col_saved_width[col] = colDefaults[col];
			ListView_SetColumnWidth(RecordsListDlg_list.handle, col, 0);
		}
	}
}

static int lv_margin_left, lv_margin_top, lv_margin_right, lv_margin_bottom;

static void ResizeListView(HWND hDlg) {
	RECT rc;
	GetClientRect(hDlg, &rc);
	int x = lv_margin_left;
	int y = lv_margin_top;
	int w = rc.right - lv_margin_left - lv_margin_right;
	int h = rc.bottom - lv_margin_top - lv_margin_bottom;
	if (w < 50) w = 50;
	if (h < 50) h = 50;
	MoveWindow(RecordsListDlg_list.handle, x, y, w, h, TRUE);
}

LRESULT CALLBACK RecordsListDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
	case WM_INITDIALOG:
		{
			RecordsListDlg = hDlg;
			RecordsListDlg_list.initialize();
			RecordsListDlg_list.handle = GetDlgItem(hDlg, LV_GLIST);

			// Calculate ListView margins from initial layout
			{
				RECT dlgRc, lvRc;
				GetClientRect(hDlg, &dlgRc);
				GetWindowRect(RecordsListDlg_list.handle, &lvRc);
				POINT lvPos = { lvRc.left, lvRc.top };
				ScreenToClient(hDlg, &lvPos);
				lv_margin_left = lvPos.x;
				lv_margin_top = lvPos.y;
				lv_margin_right = dlgRc.right - (lvPos.x + (lvRc.right - lvRc.left));
				lv_margin_bottom = dlgRc.bottom - (lvPos.y + (lvRc.bottom - lvRc.top));
			}

			// Restore saved window size
			{
				int w = nSettings::get_int((char*)"PBWinW", 0);
				int h = nSettings::get_int((char*)"PBWinH", 0);
				if (w > 200 && h > 150) {
					SetWindowPos(hDlg, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
					ResizeListView(hDlg);
				}
			}

			for (int i = 0; i < PB_NUM_COLS; i++) {
				col_visible[i] = nSettings::get_int((char*)colVisKeys[i], 1) != 0;
				col_saved_width[i] = nSettings::get_int((char*)colKeys[i], colDefaults[i]);
				if (col_saved_width[i] < 20) col_saved_width[i] = colDefaults[i];
				int w = col_visible[i] ? col_saved_width[i] : 0;
				RecordsListDlg_list.AddColumn((char*)colNames[i], w);
			}

			RecordsListDlg_list.FullRowSelect();
			g_online_mode = false;
			CheckDlgButton(hDlg, CHK_ONLINE, BST_UNCHECKED);
			SetDlgItemText(hDlg, BTN_DELETE, "Delete");
			RecordsList_Populate();

			// Default sort: date descending (newest first)
			sort_column = 0;
			sort_ascending = false;
			ListView_SortItemsEx(RecordsListDlg_list.handle, RecordsList_CompareItems, 0);

			UpdateModeRadioButtons(hDlg);

			if (g_pending_watch_session[0] != 0) {
				char sessionId[64], room[128];
				strncpy(sessionId, g_pending_watch_session, sizeof(sessionId) - 1); sessionId[sizeof(sessionId) - 1] = 0;
				strncpy(room, g_pending_watch_room, sizeof(room) - 1); room[sizeof(room) - 1] = 0;
				g_pending_watch_session[0] = 0;
				g_pending_watch_room[0] = 0;
				player_watch_begin(sessionId, room);
			}

		}
		break;
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED)
			ResizeListView(hDlg);
		break;
	case WM_CLOSE:
		SavePlaybackState(hDlg);
		EndDialog(hDlg, 0);
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDCREFRESH:
			{
				if (g_online_mode) RecordsList_PopulateOnline();
				else RecordsList_Populate();
			}
			break;
		case BTN_PLAY:
			RecordsList_PlaySelected();
			break;
		case BTN_STOP:
			player_EndGame();
			break;
		case BTN_DELETE:
			if (g_online_mode) RecordsList_DownloadSelected();
			else RecordsList_DeleteSelected();
			break;
		case CHK_ONLINE:
			if (HIWORD(wParam) == BN_CLICKED) {
				g_online_mode = IsDlgButtonChecked(hDlg, CHK_ONLINE) == BST_CHECKED;
				SetDlgItemText(hDlg, BTN_DELETE, g_online_mode ? "Download" : "Delete");
				if (g_online_mode) RecordsList_PopulateOnline();
				else RecordsList_Populate();
			}
			break;
		case BTN_OPENFOLDER:
			{
				CreateDirectory(".\\records", 0);
				ShellExecute(hDlg, "open", ".\\records", NULL, NULL, SW_SHOWNORMAL);
			}
			break;
		case RB_MODE_P2P:
			if (player_playing) player_EndGame();
			SavePlaybackState(hDlg);
			if (activate_mode(0))
				SendMessage(hDlg, WM_CLOSE, 0, 0);
			break;
		case RB_MODE_CLIENT:
			if (player_playing) player_EndGame();
			SavePlaybackState(hDlg);
			if (activate_mode(1))
				SendMessage(hDlg, WM_CLOSE, 0, 0);
			break;
		case RB_MODE_PLAYBACK:
			if (player_playing) player_EndGame();
			SavePlaybackState(hDlg);
			if (activate_mode(2))
				SendMessage(hDlg, WM_CLOSE, 0, 0);
			break;
		};
		break;
	case WM_NOTIFY:
		if(((LPNMHDR)lParam)->code==NM_DBLCLK && ((LPNMHDR)lParam)->hwndFrom==RecordsListDlg_list.handle){
			RecordsList_PlaySelected();
		}
		if(((LPNMHDR)lParam)->code==NM_RCLICK && ((LPNMHDR)lParam)->hwndFrom==ListView_GetHeader(RecordsListDlg_list.handle)){
			ShowColumnContextMenu(hDlg);
		}
		if(((LPNMHDR)lParam)->code==LVN_COLUMNCLICK && ((LPNMHDR)lParam)->hwndFrom==RecordsListDlg_list.handle){
			NMLISTVIEW* pnmv = (NMLISTVIEW*)lParam;
			if (pnmv->iSubItem == sort_column) {
				sort_ascending = !sort_ascending;
			} else {
				sort_column = pnmv->iSubItem;
				sort_ascending = true;
			}
			ListView_SortItemsEx(RecordsListDlg_list.handle, RecordsList_CompareItems, 0);
		}
		break;
	};
	return 0;
}

void player_GUI(){
	INITCOMMONCONTROLSEX icx;
	icx.dwSize = sizeof(icx);
	icx.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
	InitCommonControlsEx(&icx);
	
	HMODULE p2p_riched_hm = LoadLibrary("riched32.dll");
	
	DialogBox(hx, (LPCTSTR)RECORDER_PLAYBACK, 0, (DLGPROC)RecordsListDlgProc);
	
	FreeLibrary(p2p_riched_hm);
}

int player_MPV(void*values,int size){
	n02_TRACE();
	if (!player_playing)
		return -1;

	if (player_watch_mode) {
		// Unchanged: growing/streaming buffer path, still on the legacy
		// PlayBackBufferC global (retry-connect Fase 1 doesn't touch this).
		if (PlayBackBuffer.ptr + 10 >= PlayBackBuffer.end)
			PlayBackBuffer_WatchRefill();
		if (PlayBackBuffer.ptr + 10 < PlayBackBuffer.end) {
			char b = PlayBackBuffer.load_char();
			if (b==0x12) {
				int l = PlayBackBuffer.load_short();
				if (l < 0) {
					player_EndGame();
					return -1;
				}
				if (l > 0)
					PlayBackBuffer.load_bytes((char*)values, l);//access error
				g_watch_frames_consumed++;
				return l;
			}
			if (b==20) {
				char playernick[100];
				PlayBackBuffer.load_str(playernick, 100);
				int pn = PlayBackBuffer.load_int();
				if (pn >= 1 && pn <= 16)
					player_was_dropped[pn - 1] = true;
				if (pn == playerno && !player_watch_mode) {
					// Recording player dropped - end playback
					player_EndGame();
					return -1;
				}
				// Other player dropped - skip, continue playback
				return player_MPV(values, size);
			}
			if (b==8) {
				char nick[100];
				char msg[500];
				PlayBackBuffer.load_str(nick, 100);
				PlayBackBuffer.load_str(msg, 500);
				infos.chatReceivedCallback(nick, msg);
				return player_MPV(values, size);
			}
		} else player_EndGame();
		return -1;
	}

	// Static local-file playback: via krec_reader.
	for (;;) {
		int len = 0;
		// `size` is the caller's per-player convention constant (e.g. 12),
		// NOT the physical capacity of `values` (RetroArch's contiguous
		// multi-player buffer, sized for every player in the room) - the
		// same bug already found and fixed for retry-connect's own reader
		// (kaillera_retryconnect.cpp's next_record(values, sizeof(g_last_frame), ...) -
		// capping at `size` here silently truncated any recording with more
		// than one player's worth of bytes, desyncing the reader's position
		// from the very first multi-player frame onward (every subsequent
		// byte gets misread as if it were mid-record). Mirror that same fix.
		int type = g_playback_reader.next_record(values, 256, &len);

		if (type == KREC_EOF) {
			player_EndGame();
			return -1;
		}

		if (type == KREC_INPUT)
			return len;

		if (type == KREC_DROP) {
			int pn = g_playback_reader.last_drop_playerno;
			if (pn >= 1 && pn <= 16)
				player_was_dropped[pn - 1] = true;
			if (pn == playerno) {
				// Recording player dropped - end playback
				player_EndGame();
				return -1;
			}
			// Other player dropped - skip, continue playback
			continue;
		}

		if (type == KREC_CHAT) {
			infos.chatReceivedCallback(g_playback_reader.last_chat_nick, g_playback_reader.last_chat_msg);
			continue;
		}

		// KREC_UNKNOWN - matches the original's "falls through, returns -1"
		return -1;
	}
}
// Rewinds within the retained window (WatchSnapshotRecord() above) - resets
// to the latest snapshot at or before `frame` (clamping to the oldest one
// retained if `frame` predates everything kept) and re-walks forward
// consuming records exactly like player_MPV()'s own watch-mode switch,
// mirroring krec_reader::seek_to_frame()'s technique. No-op if nothing has
// been snapshotted yet (e.g. called before the first refill).
static void WatchSeekToFrame(int frame) {
	if (g_watch_snapshot_count == 0)
		return;

	int best = 0;
	for (int i = 0; i < g_watch_snapshot_count; i++) {
		if (g_watch_snapshots[i].frame_index <= frame)
			best = i;
		else
			break;
	}

	PlayBackBuffer.ptr = PlayBackBuffer.buffer + g_watch_snapshots[best].offset;
	g_watch_frames_consumed = g_watch_snapshots[best].frame_index;

	char scratch[256];
	while (g_watch_frames_consumed < frame && PlayBackBuffer.ptr + 10 < PlayBackBuffer.end) {
		char b = PlayBackBuffer.load_char();
		if (b == 0x12) {
			int remaining = PlayBackBuffer.load_short();
			if (remaining < 0) break;
			while (remaining > 0) { // always consume the record's full length, even if > sizeof(scratch) - see the truncation-bug notes elsewhere in this file
				int chunk = min(remaining, (int)sizeof(scratch));
				PlayBackBuffer.load_bytes(scratch, chunk);
				remaining -= chunk;
			}
			g_watch_frames_consumed++;
		} else if (b == 20) {
			char nick[100];
			PlayBackBuffer.load_str(nick, 100);
			PlayBackBuffer.load_int();
		} else if (b == 8) {
			char nick[100], msg[500];
			PlayBackBuffer.load_str(nick, 100);
			PlayBackBuffer.load_str(msg, 500);
		} else break;
	}
}

int player_get_frame_index() {
	if (!player_playing)
		return -1;
	if (player_watch_mode)
		return g_watch_frames_consumed;
	return g_playback_reader.frame_index();
}

void player_seek_to_frame(int frame) {
	if (!player_playing)
		return;
	if (player_watch_mode) {
		WatchSeekToFrame(frame);
		return;
	}
	g_playback_reader.seek_to_frame(frame);
}

int player_get_total_frames() {
	if (!player_playing)
		return -1;
	// No fixed total during Watch Live - it's an ongoing live stream, not a
	// file with a known end. -1 same as "not in a seekable mode at all";
	// the frontend's toolbar treats that as "hide/skip the progress bar"
	// (see retroarch-k3-ffw's PlaybackToolbarPaint()).
	if (player_watch_mode)
		return -1;
	return g_playback_total_frames;
}

void player_EndGame(){
	n02_TRACE();
	player_playing = false;
	if (player_watch_mode) {
		n02_watch_stop();
		player_watch_mode = false;
		if (player_watch_ended_callback)
			player_watch_ended_callback();
	}
	// Notify emulator of any players not already dropped by the recording
	for (int i = numplayers; i >= 1; i--) {
		if (i <= 16 && player_was_dropped[i - 1])
			continue;
		if (infos.clientDroppedCallback) {
			char dropname[32];
			wsprintf(dropname, "Player %d", i);
			infos.clientDroppedCallback(dropname, i);
		}
	}
	KSSDFA.input = KSSDFA_END_GAME;
	KSSDFA.state = 0;
}
bool player_SSDSTEP(){
	n02_TRACE();
	return false;
}
void player_ChatSend(char*){
	
}
bool player_RecordingEnabled(){
	return false;
}
