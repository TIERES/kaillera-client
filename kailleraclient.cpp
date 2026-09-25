#include "kailleraclient.h"
#include <windows.h>
#include <time.h>
#include <cstdio>
#include <stdarg.h>
#include <signal.h>
#include <stdlib.h>
#include "string.h"


#include "p2p_ui.h"
#include "kaillera_ui.h"
#include "kcore/kaillera_retryconnect.h"
#include "player.h"
#include "common/krec_reader.h"

#include "common/nThread.h"
#include "common/k_socket.h"
#include "common/nSettings.h"
#include "common/n02_stream.h"
#include "common/n02_update.h"
#include "common/n02_watch.h"

#include "errr.h"
#include <limits.h>
#include <stddef.h>


#define KAILLERA_DLLEXP __declspec(dllexport) __stdcall

#define KAILLERA

#define RECORDER


HINSTANCE hx;

//void __cdecl kprintf(char * arg_0, ...) {
//	char V8[1024];
//	time_t V4 = time(0);
//	tm * ecx = localtime(&V4);
//	sprintf(V8, "%02d/%02d/%02d-%02d:%02d:%02d> %s\r\n",ecx->tm_mday,ecx->tm_mon,ecx->tm_year % 0x64,ecx->tm_hour,ecx->tm_min,ecx->tm_sec, arg_0);
//	va_list args;
//	va_start (args, arg_0);
//	vprintf (V8, args);
//	va_end (args);
//}

//void OutputHexx(const void * inb, int len, bool usespace){
//	char outbb[2000];
//	char * outb = outbb;
//	char HEXDIGITS [] = {
//		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
//	};
//	char * xx = (char*)inb;
//	for (int x = 0; x <	len; x++) {
//		int dx = *xx++;
//		int hib = (dx & 0xF0)>>4;
//		int lob = (dx & 0x0F);
//		*outb++ = HEXDIGITS[hib];
//		*outb++ = HEXDIGITS[lob];
//		if (usespace)
//			*outb++ = ' ';
//	}
//	*outb = 0;
//	printf(outbb);
//}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

kailleraInfos infos_copy;

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
#ifdef KAILLERA
typedef struct {
	void (*GUI)();
	bool (*SSDSTEP)();
	int  (*MPV)(void*,int);
	void (*ChatSend)(char*);
	void (*EndGame)();
	bool (*RecordingEnabled)();
	bool (*IsHost)();
	bool (*StreamingEnabled)();
	void (*ConfigureStream)();
	void (*GetOwnerName)(char*, int);
}n02_MODULE;

static bool mod_never_host() { return false; }
static bool mod_never_stream() { return false; }
static void mod_no_stream_config() {}
static void mod_no_owner_name(char* out, int cap) { if (cap > 0) out[0] = 0; }

n02_MODULE active_mod;

n02_MODULE mod_kaillera;
n02_MODULE mod_p2p;
n02_MODULE mod_playback;
int mod_active_no = -1;
int active_mod_index;
bool mod_rerun;

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
#ifdef RECORDER
char convert[256];
HANDLE out = INVALID_HANDLE_VALUE;
char recording_player_names[4][32];

class RecordingBufferC {
public:
	char buffer[500];

	char* position;

	void reset(){
		position = buffer;
	}

	int len(){
		ptrdiff_t diff = position - buffer;
		if (diff < 0)
			return 0;
		if (diff > INT_MAX)
			return INT_MAX;
		return (int)diff;
	}
	void put_char(char x){
		*position++ = x;
	}

	void put_short(short x){
		*(short*)position = x;
		position += 2;
	}

	void put_bytes(char* x, int len){
		for (int t=0;t<len;t++){
			*position++ = *x++;
		}
	}
	void write(){
		DWORD written;
		WriteFile(out, buffer, len(), &written, NULL);
		reset();
	}
}RecordingBuffer;

static void close_recording() {
	if (out != INVALID_HANDLE_VALUE) {
		if (RecordingBuffer.len() > 0)
			RecordingBuffer.write();
		CloseHandle(out);
		out = INVALID_HANDLE_VALUE;
	}
	n02_stream_end_session();
}

// "Sem M. Card" for the game starting now (1 = no memory card, 0 = each
// player's own) - shared by the recording below and kailleraGetNoMemoryCard(),
// so what RetroArch plays with and what the replay says always agree:
//  - retry-connect: whatever the replay being resumed was played with;
//  - Server-mode room / P2P connection: that dialog's checkbox
//    (kaillera_ui.cpp / p2p_ui.cpp);
//  - Playback / Watch Live: what the replay/stream says (player.cpp).
static int CurrentGameNoMemcard() {
	if (kaillera_retryconnect_active())
		return kaillera_retryconnect_no_memcard();
	if (get_active_mode_index() == 2 || player_is_watching())
		return player_get_no_memcard();
	if (get_active_mode_index() == 1)
		return kaillera_NoMemcardEnabled() ? 1 : 0;
	return p2p_NoMemcardEnabled() ? 1 : 0;
}

int WINAPI _gameCallback(char *game, int player, int numplayers){

	close_recording();

	char GameName[128];
	strncpy(GameName, (game != NULL) ? game : "", sizeof(GameName) - 1);
	GameName[sizeof(GameName) - 1] = 0;

	const int noMemcard = CurrentGameNoMemcard();
	const char* memcardMarker = noMemcard ? N02_MEMCARD_MARKER_ON : N02_MEMCARD_MARKER_OFF;

	if (active_mod.RecordingEnabled()) {
		n02_TRACE();
		RecordingBuffer.reset();

		char FileName[2000];
		CreateDirectory("records", 0);

		// Build filename: YYMMDDHHMMSS_<SemMCard|ComMCard>.krec - player/game
		// info is in the KRC1 header; the tag says how to play it back (see
		// N02_MEMCARD_TAG_* in krec_reader.h)
		time_t t = time(0);
		tm * lt = localtime(&t);
		char datePart[16];
		strftime(datePart, sizeof(datePart), "%y%m%d%H%M%S", lt);

		wsprintf(FileName,".\\records\\%s_%s.krec", datePart,
			noMemcard ? N02_MEMCARD_TAG_ON : N02_MEMCARD_TAG_OFF);


		for(unsigned int x=0; x < strlen(FileName); x++){
			FileName[x] = convert[FileName[x]];
		}

		//open file
		out = CreateFile(FileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

		if(out==INVALID_HANDLE_VALUE){
			wsprintf(FileName+1000, "recording %s failed error = %i", FileName, GetLastError());
			MessageBox(0,FileName+1000, "recording - error",0);
		}

		DWORD written;

		WriteFile(out, "KRC1", 4, &written, NULL);
		WriteFile(out, infos_copy.appName, 128, &written, NULL);
		WriteFile(out, GameName, 128, &written, NULL);
		time_t mytime = time(NULL);
		WriteFile(out, (char*)&mytime, 4, &written, NULL);
		WriteFile(out, (char*)&player, 4, &written, NULL);
		WriteFile(out, (char*)&numplayers, 4, &written, NULL);
		WriteFile(out, (char*)recording_player_names, 128, &written, NULL);

		// First record: the "Sem M. Card" marker - see krec_reader.h.
		RecordingBuffer.put_char(8);
		RecordingBuffer.put_bytes((char*)N02_MEMCARD_MARKER_NICK, (int)strlen(N02_MEMCARD_MARKER_NICK) + 1);
		RecordingBuffer.put_bytes((char*)memcardMarker, (int)strlen(memcardMarker) + 1);
		RecordingBuffer.write();
		n02_TRACE();
	}

	if (active_mod.StreamingEnabled() && active_mod.IsHost()) {
		active_mod.ConfigureStream();
		char ownerName[64];
		active_mod.GetOwnerName(ownerName, sizeof(ownerName));
		n02_stream_start_session(infos_copy.appName, GameName, player, numplayers, recording_player_names, ownerName);
		// Same first record for spectators and the online replay.
		n02_stream_push_chat(N02_MEMCARD_MARKER_NICK, memcardMarker);
	}

	if (infos_copy.gameCallback)
		return infos_copy.gameCallback(game, player, numplayers);
	return 0;

}

	void WINAPI _chatReceivedCallback(char *nick, char *text){
		if (out != INVALID_HANDLE_VALUE) {
			RecordingBuffer.put_char(8);
			{
				size_t nickLen = strlen(nick) + 1;
				size_t textLen = strlen(text) + 1;
				int nickLenI = (nickLen > (size_t)INT_MAX) ? INT_MAX : (int)nickLen;
				int textLenI = (textLen > (size_t)INT_MAX) ? INT_MAX : (int)textLen;
				RecordingBuffer.put_bytes(nick, nickLenI);
				RecordingBuffer.put_bytes(text, textLenI);
			}
		}
		n02_stream_push_chat(nick, text);
		if (infos_copy.chatReceivedCallback)
			infos_copy.chatReceivedCallback(nick, text);
	}
	void WINAPI _clientDroppedCallback(char *nick, int playernb){
		if (out != INVALID_HANDLE_VALUE) {
			RecordingBuffer.put_char(20);
			{
				size_t nickLen = strlen(nick) + 1;
				int nickLenI = (nickLen > (size_t)INT_MAX) ? INT_MAX : (int)nickLen;
				RecordingBuffer.put_bytes(nick, nickLenI);
			}
			RecordingBuffer.put_bytes((char*)&playernb, 4);
		}
		n02_stream_push_drop(nick, playernb);
		if (infos_copy.clientDroppedCallback)
			infos_copy.clientDroppedCallback(nick, playernb);
	}




#endif
#endif

void initialize_mode_cb(HWND hDlg){
#ifdef KAILLERA
	SendMessage(hDlg, CB_ADDSTRING, 0, (WPARAM)"1. P2P");
	SendMessage(hDlg, CB_ADDSTRING, 0, (WPARAM)"2. Server");
	SendMessage(hDlg, CB_ADDSTRING, 0, (WPARAM)"3. Playback");
	SendMessage(hDlg, CB_SETCURSEL, mod_active_no, 0);
	active_mod_index = mod_active_no;
#else
	SendMessage(hDlg, CB_ADDSTRING, 0, (WPARAM)"P2P");
	SendMessage(hDlg, CB_SETCURSEL, 0, 0);
#endif
}

bool activate_mode(int mode){
#ifdef KAILLERA
	if (mode != mod_active_no) {
		mod_active_no = mode;
		active_mod_index = mode;
		mod_rerun = true;

		if (mode == 0)
			active_mod = mod_p2p;
		else if (mode == 1)
			active_mod = mod_kaillera;
		else
			active_mod = mod_playback;

		return true;
	}
#endif
	return false;
}

int get_active_mode_index(){
	return active_mod_index;
}

void loadSettings() {
	// Always start in Server mode - don't remember whatever mode (e.g.
	// Playback, entered via the lobby's "Watch" feature) was active when
	// the client last shut down.
	active_mod_index = 1;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
char * gamelist = 0;




int KSSDFAST [16] = {
	0, 0, 1, 1,
		1, 0, 1, 0,
		2, 0, 2, 0,
		3, 3, 3, 3
};

KSSDFA_ KSSDFA;


class GuiThread : public nThread {
public:
	bool running;
	void run(void) {

		running = true;

#ifdef KAILLERA

		/*

		BYTE kst[256];
		GetKeyboardState(kst);

		if (kst[VK_CAPITAL]==0)
			active_mod = mod_p2p;
		else
			active_mod = mod_kaillera;

		*/

		__try {
			mod_rerun = true;
			do {
				mod_rerun = false;
				active_mod.GUI();
			} while (mod_rerun);
		}
		__except (n02ExceptionFilterFunction(GetExceptionInformation())) {
			MessageBox(GetActiveWindow(), "Please reopen kaillera", "GUI thread exception", 0);
		}

#else
		p2p_GUI();
#endif

		KSSDFA.state = 3;
		running = false;
		//*/
	}
	void st(void * parent) {
		KSSDFA.state = create()!=0? 0:3;
	}
} gui_thread;



extern "C" {
	kailleraInfos infos;

	void KAILLERA_DLLEXP kailleraGetVersion(char *version){
		memcpy(version, "0.9", 4);
	}
	int KAILLERA_DLLEXP kailleraInit(){
		k_socket::Initialize();
		loadSettings();

#ifdef KAILLERA

		mod_playback.MPV = player_MPV;
		mod_playback.GUI = player_GUI;
		mod_playback.EndGame = player_EndGame;
		mod_playback.SSDSTEP = player_SSDSTEP;
		mod_playback.ChatSend = player_ChatSend;
		mod_playback.RecordingEnabled = player_RecordingEnabled;
		mod_playback.IsHost = mod_never_host;
		mod_playback.StreamingEnabled = mod_never_stream;
		mod_playback.ConfigureStream = mod_no_stream_config;
		mod_playback.GetOwnerName = mod_no_owner_name;

		mod_kaillera.GUI = kaillera_GUI;
		mod_kaillera.SSDSTEP = kaillera_SelectServerDlgStep;
		mod_kaillera.MPV = kaillera_modify_play_values;
		mod_kaillera.ChatSend = kaillera_game_chat_send;
		mod_kaillera.EndGame = kaillera_end_game;
		mod_kaillera.RecordingEnabled = kaillera_RecordingEnabled;
		mod_kaillera.IsHost = kaillera_IsHost;
		mod_kaillera.StreamingEnabled = kaillera_StreamingEnabled;
		mod_kaillera.ConfigureStream = kaillera_ConfigureStream;
		mod_kaillera.GetOwnerName = kaillera_GetOwnerName;

		mod_p2p.GUI = p2p_GUI;
		mod_p2p.SSDSTEP = p2p_SelectServerDlgStep;
		mod_p2p.MPV = p2p_modify_play_values;
		mod_p2p.ChatSend = p2p_send_chat;
		mod_p2p.EndGame = p2p_EndGame;
		mod_p2p.RecordingEnabled = p2p_RecordingEnabled;
		mod_p2p.IsHost = p2p_IsHost;
		mod_p2p.StreamingEnabled = p2p_StreamingEnabled;
		mod_p2p.ConfigureStream = p2p_ConfigureStream;
		mod_p2p.GetOwnerName = p2p_GetOwnerName;

		activate_mode(active_mod_index);

#ifdef RECORDER


		{
			for(int x = 0 ; x < 256; x++){
				convert[x] = '_';
			}
		}
		{
			for(char x = 'A' ; x <= 'Z'; x++){
				convert[x] = x;
			}
		}
		{
			for(char x = '0' ; x <= '9'; x++){
				convert[x] = x;
			}
		}
		{
			for(char x = 'a' ; x <= 'z'; x++){
				convert[x] = x;
			}
		}
			convert['['] = '[';
			convert[0] = 0;
			convert[']'] = ']';
			convert['.'] = '.';
			convert['\\'] = '\\';
			convert['-'] = '-';



#endif



#endif

		n02_update_check_and_prompt();

		return 0;
	}
	void KAILLERA_DLLEXP kailleraShutdown(){
		k_socket::Cleanup();
		if (gamelist != 0)
			free(gamelist);
		gamelist = 0;
#ifdef KAILLERA
#ifdef RECORDER
		close_recording();
		n02_stream_shutdown();
#endif
#endif
	}
			void KAILLERA_DLLEXP kailleraSetInfos(kailleraInfos *infos_){
				infos = *infos_;
				strncpy(APP, (infos.appName != NULL) ? infos.appName : "", 127);
				APP[127] = 0;

				if (gamelist != 0)
					free(gamelist);
			gamelist = 0;

			char * xx = infos.gameList;
			size_t l = 0;
			if (xx != 0) {
				size_t p;
				while ((p=strlen(xx))!= 0){
					l += p + 1;
					xx += p + 1;
				}
				l++;
				gamelist = (char*)malloc(l);
			memcpy(gamelist, infos.gameList, l);
		}
		infos.gameList = gamelist;
		infos_copy = infos;
#ifdef KAILLERA
#ifdef RECORDER
		infos.chatReceivedCallback = _chatReceivedCallback;
		infos.clientDroppedCallback = _clientDroppedCallback;
		infos.gameCallback = _gameCallback;
#endif
#endif
	}
	void KAILLERA_DLLEXP kailleraSelectServerDialog(void* parent){
		KSSDFA.state = 0;
		KSSDFA.input = 0;

		gui_thread.st(parent);

		nThread game_thread;
		game_thread.capture();
		game_thread.sleep(1);

		if (gui_thread.running) {
			do {
				__try {
					while (KSSDFA.state != 3) {
						int prev_state = KSSDFA.state;
						KSSDFA.state = KSSDFAST[((KSSDFA.state) << 2) | KSSDFA.input];
						KSSDFA.input = 0;
#ifdef RECORDER
						if (prev_state == 2 && KSSDFA.state != 2) {
							close_recording();
						}
#endif
						if (KSSDFA.state == 2) {
							MSG message;
							while (PeekMessage(&message, 0, 0, 0, PM_REMOVE)) {
								TranslateMessage(&message);
								DispatchMessage(&message);
							}
						}
						else if (KSSDFA.state == 1) {
							//kprintf("call gamecallback");
							KSSDFA.state = 2;
							if (infos.gameCallback)
								infos.gameCallback(GAME, playerno, numplayers);
						}
						else if (KSSDFA.state == 0) {
#ifdef RECORDER
							close_recording();
#endif
							MSG message;
							while (PeekMessage(&message, 0, 0, 0, PM_REMOVE)) {
								TranslateMessage(&message);
								DispatchMessage(&message);
							}
#ifndef KAILLERA
							if (p2p_SelectServerDlgStep()) {
#else
							if (active_mod.SSDSTEP()) {
#endif
								continue;
							}
						}

						// Use MsgWaitForMultipleObjects instead of sleep to avoid
						// sluggish window dragging while still yielding CPU
						MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT);
					}
				}
				__except (n02ExceptionFilterFunction(GetExceptionInformation())) {
					KSSDFA.state = 0;
				}
			} while (KSSDFA.state != 3);
		}
	}

	int  KAILLERA_DLLEXP kailleraModifyPlayValues(void *values, int size){
		//kaillera_core_debug("x");
		if (KSSDFA.state==2) {
#ifdef KAILLERA

#ifdef RECORDER

			short siz = active_mod.MPV(values, size);

			if (out != INVALID_HANDLE_VALUE) {
				RecordingBuffer.put_char(0x12);
				RecordingBuffer.put_short(siz);
				RecordingBuffer.put_bytes((char*)values, siz);
				RecordingBuffer.write();
			}
			n02_stream_push_frame(values, siz);
			return siz;

#else
			return active_mod.MPV(values, size);
#endif

#else
			return p2p_modify_play_values(values, size);
#endif
		} else return -1;
	}
	void KAILLERA_DLLEXP kailleraChatSend(char *text){
#ifdef KAILLERA
		active_mod.ChatSend(text);
#else
		p2p_ui_chat_send(text);
#endif
	}
	void KAILLERA_DLLEXP kailleraEndGame(){
#ifdef KAILLERA
#ifdef RECORDER
		close_recording();
#endif
		// "Acompanhar ao vivo!" never switches active_mod (see
		// kaillera_modify_play_values()) - active_mod.EndGame() would run
		// mod_kaillera's own EndGame (sends GAMRDROP for a "game" the server
		// was never told about) instead of the watch-mode cleanup that
		// actually applies here.
		if (player_is_watching())
			player_EndGame();
		else
			active_mod.EndGame();
#else
		p2p_EndGame();
#endif

	}

	/* Custom extension, not part of the standard Kaillera client API - frontends
	   should GetProcAddress() this optionally (a real/older kailleraclient.dll
	   won't export it) and treat a missing symbol as "not in playback mode".
	   Lets the frontend tell Playback mode (static .krec file, "Replays Online",
	   or Watch Live) apart from P2P/Server: unlike those two, Playback has no
	   live peer to stay in lockstep with, so it's safe to allow fast-forward/
	   frame-advance hotkeys the frontend would otherwise disable while any
	   Kaillera session is active. Returns 1 in Playback mode, 0 otherwise -
	   also 1 during a retry-connect group replay (kcore/kaillera_retryconnect.h)
	   and while "Acompanhar ao vivo!" is spectating without leaving the room
	   (player_is_watching(), player.cpp) - both stay on active_mod ==
	   mod_kaillera (get_active_mode_index() == 1) now, but have exactly the
	   same "no live peer to stay in lockstep with" property Playback mode
	   does, so fast-forward needs to stay allowed for them too. */
	int KAILLERA_DLLEXP kailleraIsPlaybackMode(){
		return get_active_mode_index() == 2 || kaillera_retryconnect_active() || player_is_watching();
	}

	/* "Sem M. Card" for the game starting now (see CurrentGameNoMemcard())
	   - optional export, same GetProcAddress()-if-present convention as
	   kailleraIsPlaybackMode above; the frontend reads it from its game
	   callback. 1 = start with no memory card in either slot, 0 = each
	   player keeps their own cards. */
	int KAILLERA_DLLEXP kailleraGetNoMemoryCard(){
		return CurrentGameNoMemcard();
	}

	/* retry-connect (kcore/kaillera_retryconnect.h) - optional exports, same
	   GetProcAddress()-if-present convention as kailleraIsPlaybackMode above.
	   A frontend (retroarch-k3-ffw) uses these to let the room's host drive a
	   group replay with RetroArch's own native Pause/Enter keys instead of a
	   custom UI, and to relay that to every other player. */

	/* True only for the room's host while a retry-connect session is active -
	   the frontend must check this before letting local Pause/Enter do
	   anything during a Kaillera session (a peer's local presses must not
	   affect anyone). */
	int KAILLERA_DLLEXP kailleraRetryConnectCanControl(){
		return kaillera_retryconnect_can_control() ? 1 : 0;
	}

	/* Call when the local user (already confirmed to be the host via
	   kailleraRetryConnectCanControl() above) presses native Pause/Resume, or
	   Enter to go live. action is one of RC_ACTION_PAUSE/RESUME/GO_LIVE
	   (kcore/k_instruction.h); frame_index is the frontend's own authoritative
	   frame counter at that moment - relayed to every other player unchanged. */
	void KAILLERA_DLLEXP kailleraRetryConnectNotifyLocalControl(int action, int frame_index){
		kaillera_retryconnect_notify_local_control(action, frame_index);
	}

	/* Call once per frontend tick, including while natively paused (this is
	   the only channel retry-connect has to reach a paused client -
	   kailleraModifyPlayValues() itself stops being called while paused, so
	   its own drain never runs then; this export drains the connection on
	   its own for exactly that reason). Returns 1 and fills
	   *outAction/*outFrameIndex exactly once per remote action received; 0
	   when there's nothing new. */
	int KAILLERA_DLLEXP kailleraRetryConnectPoll(int* outAction, int* outFrameIndex){
		kaillera_retryconnect_pump();
		return kaillera_retryconnect_poll(outAction, outFrameIndex) ? 1 : 0;
	}

	/* True for every client (host or peer) while a retry-connect session is
	   active - unlike kailleraRetryConnectCanControl() above, does not imply
	   "is host". Lets the frontend tell a retry-connect peer apart from
	   someone using n02's ordinary standalone Playback/Watch mode
	   (kailleraIsPlaybackMode() is true in both cases, but only the former
	   has a host to defer to) - needed so a peer's own fast-forward hotkey
	   can be suppressed during a group replay while solo playback keeps
	   fast-forwarding freely. */
	int KAILLERA_DLLEXP kailleraRetryConnectActive(){
		return kaillera_retryconnect_active() ? 1 : 0;
	}

	/* Host-only fast-forward handoff (kcore/kaillera_retryconnect.h). `data`/
	   `size` is a full core savestate the frontend just took the instant it
	   stopped fast-forwarding - uploaded to the community server and relayed
	   to every other player as RC_ACTION_STATE_READY so they can jump
	   straight there instead of trying to reach the same frame by
	   fast-forwarding themselves (not reliable across different
	   machines/cores). No-op if not host. */
	void KAILLERA_DLLEXP kailleraRetryConnectUploadState(const void* data, int size){
		kaillera_retryconnect_upload_state(data, size);
	}

	/* Downloads the state kailleraRetryConnectUploadState() above just
	   uploaded into outBuffer (capacity bufferCap - the frontend must size
	   this from its own core_serialize_size(), matching the host's since
	   everyone runs the same core/content). Returns the byte count written,
	   ready to hand to core_unserialize(), or -1 on any failure. */
	int KAILLERA_DLLEXP kailleraRetryConnectDownloadState(void* outBuffer, int bufferCap, int* outFrameIndex){
		return kaillera_retryconnect_download_state(outBuffer, bufferCap, outFrameIndex);
	}

	/* Host-only local rewind support (toolbar "Rebobinar" button) - see
	   kcore/kaillera_retryconnect.h's own doc comments for the exact 3-step
	   flow these are meant to be used in (restore a local checkpoint, seek,
	   then re-use kailleraRetryConnectUploadState() above to broadcast it). */
	int KAILLERA_DLLEXP kailleraRetryConnectGetFrameIndex(){
		return kaillera_retryconnect_get_frame_index();
	}
	int KAILLERA_DLLEXP kailleraRetryConnectGetTotalFrames(){
		return kaillera_retryconnect_get_total_frames();
	}
	void KAILLERA_DLLEXP kailleraRetryConnectSeekLocal(int frame_index){
		kaillera_retryconnect_seek_local(frame_index);
	}

	/* Checkpoint-based rewind (solo "Reproducao de Replay", not retry-connect
	   or Watch Live) - see player.h. -1 outside static local-file playback. */
	int KAILLERA_DLLEXP kailleraPlaybackGetFrameIndex(){
		return player_get_frame_index();
	}
	void KAILLERA_DLLEXP kailleraPlaybackSeekToFrame(int frame){
		player_seek_to_frame(frame);
	}
	int KAILLERA_DLLEXP kailleraPlaybackGetTotalFrames(){
		return player_get_total_frames();
	}
	/* Same action as the Player dialog's own "Stop" button
	   (player.cpp, BTN_STOP) - calls player_EndGame() directly. */
	void KAILLERA_DLLEXP kailleraPlaybackStop(){
		player_EndGame();
	}

	/* "Ir direto para o Ao Vivo!" (Watch Live toolbar) - spectator-side.
	   Marks/checks a pending request and downloads the state the host
	   uploads in response - see common/n02_watch.h's own doc comments for
	   the exact flow these three are meant to be called in. */
	int KAILLERA_DLLEXP kailleraWatchRequestState(){
		return n02_watch_request_state() ? 1 : 0;
	}
	int KAILLERA_DLLEXP kailleraWatchStateReady(){
		return n02_watch_state_ready() ? 1 : 0;
	}
	int KAILLERA_DLLEXP kailleraWatchDownloadState(void* outBuffer, int bufferCap, int* outFrameIndex, int* outByteOffset){
		return n02_watch_download_state(outBuffer, bufferCap, outFrameIndex, outByteOffset);
	}
	/* Call right after core_unserialize()ing the state kailleraWatchDownloadState()
	   above just returned, passing the same outFrameIndex/outByteOffset it
	   gave back - see player_watch_jump_to_live()'s own comment (player.cpp)
	   for why this needs to be a distinct call from the rewind-style seek
	   kailleraPlaybackSeekToFrame() above does. */
	void KAILLERA_DLLEXP kailleraWatchJumpToLive(int frameIndex, int byteOffset){
		if (player_watch_state_was_sent()) {
			kaillera_error_callback("Essa funcao esta desabilitada ate que voce aperte Pause ou Rebobinar!");
			return;
		}
		player_watch_jump_to_live(frameIndex, byteOffset);
	}

	/* "Ir direto para o Ao Vivo!" - host-side. See common/n02_stream.h's own
	   doc comments; call kailleraStreamCheckStateRequested() every frame
	   while hosting with streaming enabled (never blocks - the network poll
	   runs on a background thread - so this is cheap to call
	   unconditionally) and, when it returns nonzero, take a retro_serialize()
	   and hand it to kailleraStreamUploadState() right away, before that
	   frame's kailleraModifyPlayValues() (the upload itself also happens in
	   the background). */
	int KAILLERA_DLLEXP kailleraStreamCheckStateRequested(){
		return n02_stream_check_state_requested() ? 1 : 0;
	}
	void KAILLERA_DLLEXP kailleraStreamUploadState(int frameIndex, const void* data, int size){
		n02_stream_upload_state(frameIndex, data, size);
	}
};


BOOL APIENTRY DllMain (HINSTANCE hInst,
	DWORD reason,
	LPVOID){
	if(reason==DLL_PROCESS_ATTACH)
		hx = hInst;
	return TRUE;
}


extern "C" {
	__declspec(dllexport) int z00_With = 0;
	__declspec(dllexport) int z01_stupidity = 0;
	__declspec(dllexport) int z02_even = 0;
	__declspec(dllexport) int z03_the = 0;
	__declspec(dllexport) int z04_gods = 0;
	__declspec(dllexport) int z05_contend = 0;
	__declspec(dllexport) int z06_in = 0;
	__declspec(dllexport) int z07_vain = 0;
}
