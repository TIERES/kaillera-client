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
#include "common/n02_wecamp.h"
#include "common/n02_mcchoice.h"

void __cdecl kaillera_ui_gdebug(char* arg_0, ...); // kaillera_ui.cpp - room chat line
void __cdecl outpf(char * arg_0, ...); // p2p_ui.cpp - P2P connection window line

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

// The live room's / P2P connection's memory card of each slot for the game
// starting now (N02_MEMCARD_CHOICE_*, common/n02_mcchoice.h) - each player
// picks their own slot's in that dialog (kaillera_ui.cpp / p2p_ui.cpp).
// Taken once per game, at the top of _gameCallback().
static int g_game_mc_slots[2] = { N02_MEMCARD_CHOICE_NONE, N02_MEMCARD_CHOICE_NONE };
static void CurrentGameMcSlots(int slot[2]) {
	slot[0] = g_game_mc_slots[0];
	slot[1] = g_game_mc_slots[1];
}
static bool CurrentGameHasMcSlot() {
	return g_game_mc_slots[0] != N02_MEMCARD_CHOICE_NONE || g_game_mc_slots[1] != N02_MEMCARD_CHOICE_NONE;
}

// "Sem M. Card" for the game starting now (1 = no memory card, 0 = each
// player's own) - shared by the recording below and kailleraGetNoMemoryCard(),
// so what RetroArch plays with and what the replay says always agree:
//  - retry-connect: whatever the replay being resumed was played with;
//  - Server-mode room / P2P connection: no WE Camp card in either slot
//    (CurrentGameMcSlots());
//  - Playback / Watch Live: what the replay/stream says (player.cpp).
static int CurrentGameNoMemcard() {
	if (kaillera_retryconnect_active())
		return kaillera_retryconnect_no_memcard();
	if (get_active_mode_index() == 2 || player_is_watching())
		return player_get_no_memcard();
	return CurrentGameHasMcSlot() ? 0 : 1;
}

// Memory card setup for the game starting now: 2 = WE Camp cards ("M. Card
// online" / "M. Card da ISO" in at least one slot - the fork only needs to
// know they come from kailleraMemcardPrepare()), 1 = each player's own, 0 =
// none. Same sources as CurrentGameNoMemcard(); a replay/stream/retry-connect
// is "online" when it carries the online marker (krec_reader.h). "Each
// player's own" is only what an old replay may still say.
#define N02_MEMCARD_NONE   0
#define N02_MEMCARD_OWN    1
#define N02_MEMCARD_ONLINE 2
static int CurrentGameMemcardMode() {
	char sha[2][65];
	if (kaillera_retryconnect_active()) {
		if (kaillera_retryconnect_mconline(sha))
			return N02_MEMCARD_ONLINE;
		return kaillera_retryconnect_no_memcard() ? N02_MEMCARD_NONE : N02_MEMCARD_OWN;
	}
	if (get_active_mode_index() == 2 || player_is_watching()) {
		if (player_get_mconline(sha))
			return N02_MEMCARD_ONLINE;
		return player_get_no_memcard() ? N02_MEMCARD_NONE : N02_MEMCARD_OWN;
	}
	return CurrentGameHasMcSlot() ? N02_MEMCARD_ONLINE : N02_MEMCARD_NONE;
}

// "MultiTap" for the game starting now (1 = RetroArch may plug a PSX Multitap
// into PCSX ReARMed when the room has 3+ players, 0 = never) - same sources
// as CurrentGameNoMemcard() above. P2P is always 2 players, so 0 there.
static int CurrentGameMultiTap() {
	if (kaillera_retryconnect_active())
		return kaillera_retryconnect_multitap();
	if (get_active_mode_index() == 2 || player_is_watching())
		return player_get_multitap();
	if (get_active_mode_index() == 1)
		return kaillera_MultiTapEnabled() ? 1 : 0;
	return 0;
}

/* Discord voice channel of the match (wg-camp's /api/voice/join, see
   common/n02_wecamp.h). At the start of a live game (Server room or P2P,
   2+ players) a logged-in player's DLL asks wg-camp for the room's voice
   channel in the WE Camp Discord: wg-camp moves the player into it when
   they're already in one of the server's voice channels; otherwise the
   channel is opened in the Discord app (once per channel - "Entrar" is one
   click). Each player's own DLL asks for itself - nobody can pull anybody
   else into a call. Only with "Participar da chamada de voz" checked in the
   M. Card Online dialog (n02_wecamp_voice_enabled()); unchecked, the room
   is told in the chat that this player stays out of the call.
   The channel only exists once 2 players of the room asked for it: until
   then wg-camp answers "waiting" and this DLL asks again every few seconds
   (the second request moves both). */
static void VoiceLog(const char* fmt, ...) {
	char msg[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
	va_end(args);
	if (get_active_mode_index() == 0)
		outpf((char*)"%s", msg);
	else
		kaillera_ui_gdebug((char*)"%s", msg);
}

static char g_voice_opened[160];      // channel link already opened this session
static volatile LONG g_voice_game = 0; // bumped at every game start - stops an older wait

#define N02_VOICE_RETRY_MS 10000
#define N02_VOICE_RETRIES 12       // ~2 minutes waiting for a second player

struct VoiceJoinJob {
	char players[8 * 32 + 8];
	LONG game;
};

static DWORD WINAPI VoiceJoinThread(LPVOID param) {
	VoiceJoinJob* job = (VoiceJoinJob*)param;
	char status[32], url[160], appUrl[160], code[64], err[300];
	bool ok;
	for (int attempt = 0; ; attempt++) {
		ok = n02_wecamp_voice_join(job->players, status, sizeof(status), url, sizeof(url),
			appUrl, sizeof(appUrl), code, sizeof(code), err, sizeof(err));
		if (!ok || strcmp(status, "waiting") != 0)
			break;
		if (attempt >= N02_VOICE_RETRIES) {
			VoiceLog("* Discord: ninguem mais da sala esta na chamada de voz.");
			delete job;
			return 0;
		}
		Sleep(N02_VOICE_RETRY_MS);
		if (g_voice_game != job->game) {
			delete job; // another game started - its own request takes over
			return 0;
		}
	}
	delete job;
	if (!ok) {
		if (strcmp(code, "not_linked") == 0) {
			// Unlinked on the site since the last check - the dialog's box
			// follows (greyed out) after this.
			n02_wecamp_refresh_discord();
			VoiceLog("* Discord: sua conta WE Camp nao tem mais Discord vinculado - vincule em %s/conta.", "https://" N02_WECAMP_HOST);
		} else if (strcmp(code, "disabled") != 0) {
			VoiceLog("* Discord: canal de voz indisponivel (%s).", err[0] ? err : code);
		}
		return 0;
	}
	if (strcmp(status, "moved") == 0) {
		VoiceLog("* Discord: voce esta no canal de voz da partida.");
	} else if (strcmp(g_voice_opened, url) != 0) {
		strncpy(g_voice_opened, url, sizeof(g_voice_opened) - 1);
		if ((INT_PTR)ShellExecuteA(NULL, "open", appUrl, NULL, NULL, SW_SHOWNOACTIVATE) <= 32)
			ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNOACTIVATE);
		VoiceLog("* Discord: canal de voz da partida aberto no Discord - clique em \"Entrar\". "
			"Fique no canal \"Aguardando partida\" do Discord da WE Camp para entrar sozinho nas proximas.");
	}
	return 0;
}

static void VoiceJoinStart(int numplayers) {
	if (numplayers < 2 || !n02_wecamp_logged_in())
		return;
	if (kaillera_retryconnect_active() || player_is_watching())
		return;
	int mode = get_active_mode_index();
	if (mode != 0 && mode != 1)
		return;
	char nick[32];
	active_mod.GetOwnerName(nick, sizeof(nick));
	if (_stricmp(nick, n02_wecamp_username()) != 0)
		return; // playing under another nick - not this account's match
	if (!n02_wecamp_voice_enabled()) {
		char line[160];
		_snprintf(line, sizeof(line) - 1, "* %s nao vai participar da chamada de voz do Discord (desativada em M. Card Online).", nick);
		line[sizeof(line) - 1] = 0;
		active_mod.ChatSend(line);
		return;
	}

	char names[8][32];
	int count = mode == 0 ? p2p_GetGamePlayers(names, 8) : kaillera_GetGamePlayers(names, 8);
	if (numplayers < count)
		count = numplayers;
	VoiceJoinJob* job = new VoiceJoinJob;
	job->players[0] = 0;
	for (int i = 0; i < count; i++) {
		if (i) strcat(job->players, ",");
		strncat(job->players, names[i], 31);
	}
	job->game = InterlockedIncrement(&g_voice_game);
	HANDLE t = CreateThread(NULL, 0, VoiceJoinThread, job, 0, NULL);
	if (t)
		CloseHandle(t);
	else
		delete job;
}

int WINAPI _gameCallback(char *game, int player, int numplayers){

	close_recording();

	char GameName[128];
	strncpy(GameName, (game != NULL) ? game : "", sizeof(GameName) - 1);
	GameName[sizeof(GameName) - 1] = 0;

	// The live game's memory card slots, before anything below asks
	// (CurrentGameMcSlots()). P2P works them out now - the chat that came in
	// with the other end's "Ready" was delivered after it (p2p_core).
	g_game_mc_slots[0] = g_game_mc_slots[1] = N02_MEMCARD_CHOICE_NONE;
	if (get_active_mode_index() == 1)
		kaillera_GetGameMcSlots(g_game_mc_slots);
	else if (get_active_mode_index() == 0)
		p2p_GetGameMcSlots(g_game_mc_slots);

	const int noMemcard = CurrentGameNoMemcard();
	const char* memcardMarker = noMemcard ? N02_MEMCARD_MARKER_ON : N02_MEMCARD_MARKER_OFF;
	const char* multitapMarker = CurrentGameMultiTap() ? N02_MULTITAP_MARKER_ON : N02_MULTITAP_MARKER_OFF;

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
		// Second record: the "MultiTap" marker.
		RecordingBuffer.put_char(8);
		RecordingBuffer.put_bytes((char*)N02_MEMCARD_MARKER_NICK, (int)strlen(N02_MEMCARD_MARKER_NICK) + 1);
		RecordingBuffer.put_bytes((char*)multitapMarker, (int)strlen(multitapMarker) + 1);
		RecordingBuffer.write();
		n02_TRACE();
	}

	// A solo game (the host alone) streams too.
	if (active_mod.StreamingEnabled() && active_mod.IsHost()) {
		active_mod.ConfigureStream();
		char ownerName[64];
		active_mod.GetOwnerName(ownerName, sizeof(ownerName));
		n02_stream_start_session(infos_copy.appName, GameName, player, numplayers, recording_player_names, ownerName);
		// Same first record for spectators and the online replay.
		n02_stream_push_chat(N02_MEMCARD_MARKER_NICK, memcardMarker);
		n02_stream_push_chat(N02_MEMCARD_MARKER_NICK, multitapMarker);
	}

	VoiceJoinStart(numplayers);

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

	static void McFlushChat(); // M. Card online: chat lines queued by the send threads
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
		// kailleraInit() just installed a package update and RetroArch is
		// closing to reopen with it - no Kaillera window this time.
		if (n02_update_restart_pending())
			return;

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
							McFlushChat();
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
			McFlushChat();
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

	/* "MultiTap" for the game starting now (see CurrentGameMultiTap()) - same
	   optional-export convention; the frontend reads it from its game
	   callback. 1 = a PSX Multitap may be plugged in when the game has 3+
	   players (the frontend picks the port(s) from the player count), 0 =
	   never. */
	int KAILLERA_DLLEXP kailleraGetMultiTap(){
		return CurrentGameMultiTap();
	}

	/* ------------------------------------------------------------------
	   "M. Card online" - optional exports, same GetProcAddress()-if-present
	   convention. The frontend (retroarch-k3-ffw's kaillera_sync.c):
	    1. reads kailleraGetMemcardMode() from its game callback;
	    2. for an online game, right before loading the core, hashes the
	       content ("CRC32:SIZE") and calls kailleraMemcardPrepare(), which
	       writes the match's two cards into `dir` (pcsx-card1.mcd = the 1P's
	       choice, pcsx-card2.mcd = the 2P's - CurrentGameMcSlots()) - from WE
	       Camp for a live room, or the exact versions named by the replay's
	       marker for playback/Watch Live/retry-connect;
	    3. after the core is unloaded, calls kailleraMemcardFinish(): in a
	       live room the cards that changed since the last send go back (on
	       a worker thread) - by the sender right away, by the others only
	       if the sender didn't (see McFinishThread); the first send becomes
	       the cards' new version. With a server that takes a list of bases
	       each save also goes out during the match (McWatchThread).
	   "Live room" = a Server-mode room or a P2P connection; the notices go to
	   that mode's chat (McLog).
	   A slot on "M. Card da ISO" goes the same way with the game's own card of
	   that slot, shared by everyone who plays it in that mode (e.g. a
	   championship's Master League, one match at a time) - slot 2's stays
	   empty with a server from before it existed. Only the player who picked
	   it needs a WE Camp login; the others fetch and send it with the DLL's
	   shared key. A card is sent back only when the match changed it. A replay
	   of it carries the same marker.
	   ------------------------------------------------------------------ */

	static void McLog(const char* fmt, ...) {
		char msg[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
		va_end(args);
		if (get_active_mode_index() == 0)
			outpf((char*)"%s", msg);
		else
			kaillera_ui_gdebug((char*)"%s", msg);
	}

	// One live match's online cards, from kailleraMemcardPrepare() until the
	// last one went to WE Camp - on the heap, because that last send (a
	// guest's comes 15 s after the match) can still be going when the next
	// match starts.
	//
	// The core writes a card file as the game saves (pcsx_rearmed's SaveMcd,
	// 128 bytes at a time), so each save can go out during the match: when
	// the server takes a list of bases (multi_base), McWatchThread checks the
	// cards every second and, once a changed card stayed the same for a few
	// seconds (the game finished writing), the PC that sends that slot first
	// (the slot's own player, who picked its card) sends it. The others only
	// note it as a version they saw, so their send at the end still goes in
	// if that player dropped after a save.
	#define N02_MC_KNOWN_MAX 16        // card versions a send can name as its base
	#define N02_MC_WATCH_MS 1000       // how often the cards are checked during the match
	#define N02_MC_SAVE_SETTLE_MS 3000 // a save is over once its card stayed this long unchanged

	struct McSession {
		bool iso[2];               // "M. Card da ISO": the game's card of that slot (else the slot player's own)
		bool sender[2];            // sends that slot first: the slot's own player
		bool track[2];             // this PC watches and sends that slot: it has a card this PC may send
		                           // (with a login, or the DLL's key for M. Card da ISO)
		bool midMatch;             // WE Camp takes a list of bases - saves go out during the match
		char dir[MAX_PATH];
		char contentId[32];
		char players[8 * 32 + 8];
		char nick[32];
		int slots;
		bool hasCard[2];           // false: empty slot - nothing to send
		char owner[2][32];
		// The card versions this PC saw, oldest first: [0] the one the match
		// began with, then each finished save. A send names them all as its
		// base (see api_commit in wg-camp's memcards.py).
		char known[2][N02_MC_KNOWN_MAX][65];
		int knownCount[2];
		bool saved[2];                    // this PC got a save of it into WE Camp during the match
		char onServer[2][N02_MCD_SIZE];   // what WE Camp has, as far as this PC knows
		char final[2][N02_MCD_SIZE];      // the cards as the match ended
		bool finalOk[2];
		HANDLE stop;                      // signalled when the match ends
		HANDLE watcher;                   // McWatchThread (midMatch only)
	};
	static McSession* g_mcs; // the current match's - set and cleared by the thread that runs the game
	// The slots the match starts with no card inserted (bit 0 = slot 1, bit 1
	// = slot 2) - kailleraMemcardEmptySlots(), for the frontend.
	static int g_mc_empty_slots;

	static bool McWriteCard(const char* dir, int slot, const char* data) {
		char path[MAX_PATH];
		_snprintf(path, sizeof(path), "%s\\pcsx-card%d.mcd", dir, slot + 1);
		path[sizeof(path) - 1] = 0;
		HANDLE f = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		BOOL ok = WriteFile(f, data, N02_MCD_SIZE, &written, NULL);
		CloseHandle(f);
		return ok && written == N02_MCD_SIZE;
	}

	static bool McReadCard(const char* dir, int slot, char* data) {
		char path[MAX_PATH];
		_snprintf(path, sizeof(path), "%s\\pcsx-card%d.mcd", dir, slot + 1);
		path[sizeof(path) - 1] = 0;
		HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f == INVALID_HANDLE_VALUE)
			return false;
		DWORD got = 0;
		BOOL ok = ReadFile(f, data, N02_MCD_SIZE, &got, NULL);
		CloseHandle(f);
		return ok && got == N02_MCD_SIZE;
	}

	// The marker every replay/stream of this match carries (krec_reader.h).
	static void McWriteMarker(const char* sha1, const char* sha2) {
		char marker[200];
		_snprintf(marker, sizeof(marker), "%s%s %s", N02_MCONLINE_MARKER_PREFIX,
			sha1 && sha1[0] ? sha1 : N02_MCONLINE_MARKER_NOCARD, sha2 && sha2[0] ? sha2 : N02_MCONLINE_MARKER_NOCARD);
		marker[sizeof(marker) - 1] = 0;
#ifdef RECORDER
		if (out != INVALID_HANDLE_VALUE) {
			RecordingBuffer.put_char(8);
			RecordingBuffer.put_bytes((char*)N02_MEMCARD_MARKER_NICK, (int)strlen(N02_MEMCARD_MARKER_NICK) + 1);
			RecordingBuffer.put_bytes(marker, (int)strlen(marker) + 1);
			RecordingBuffer.write();
		}
#endif
		if (active_mod.StreamingEnabled() && active_mod.IsHost())
			n02_stream_push_chat(N02_MEMCARD_MARKER_NICK, marker);
	}

	// Playback / Watch Live / retry-connect: the versions the match began with.
	static int McPrepareFromMarker(const char* dir, char sha[2][65]) {
		static char buf[N02_MCD_SIZE + 1];
		for (int s = 0; s < 2; s++) {
			if (strcmp(sha[s], N02_MCONLINE_MARKER_NOCARD) == 0) {
				g_mc_empty_slots |= 1 << s; // played with no card inserted
				continue;
			}
			if (strlen(sha[s]) != 64)
				continue; // "-": an older match's empty slot - the core creates a blank card, as it did then
			if (!n02_wecamp_get_card(sha[s], buf, true) || !McWriteCard(dir, s, buf))
				return -1;
		}
		return 1;
	}

	static void McKnownAdd(McSession* m, int s, const char* sha) {
		for (int i = 0; i < m->knownCount[s]; i++)
			if (strcmp(m->known[s][i], sha) == 0)
				return;
		if (m->knownCount[s] == N02_MC_KNOWN_MAX) {
			// Keep the version the match began with, drop the oldest save after it.
			memmove(m->known[s][1], m->known[s][2], (N02_MC_KNOWN_MAX - 2) * sizeof(m->known[s][0]));
			m->knownCount[s]--;
		}
		strcpy(m->known[s][m->knownCount[s]++], sha);
	}

	static void McCardName(const McSession* m, int s, char* buf, int cap) {
		if (m->iso[s])
			_snprintf(buf, cap, "M. Card da ISO (slot %d)", s + 1);
		else
			_snprintf(buf, cap, "Memory Card de %s", m->owner[s]);
		buf[cap - 1] = 0;
	}

	// Room chat lines from the send threads. The chat goes out on the thread
	// that already talks to the network - the emulator's during the game
	// (kailleraModifyPlayValues), the main loop's outside it (McFlushChat) -
	// as the Kaillera/P2P message queues aren't safe from any other thread.
	static SRWLOCK g_mcChatLock = SRWLOCK_INIT;
	static char g_mcChat[4][128];
	static volatile LONG g_mcChatCount;

	static void McQueueChat(const char* text) {
		AcquireSRWLockExclusive(&g_mcChatLock);
		if (g_mcChatCount < 4) {
			strncpy(g_mcChat[g_mcChatCount], text, sizeof(g_mcChat[0]) - 1);
			g_mcChat[g_mcChatCount][sizeof(g_mcChat[0]) - 1] = 0;
			g_mcChatCount++;
		}
		ReleaseSRWLockExclusive(&g_mcChatLock);
	}

	static void McFlushChat() {
		if (g_mcChatCount == 0)
			return;
		char lines[4][128];
		int n;
		AcquireSRWLockExclusive(&g_mcChatLock);
		n = g_mcChatCount;
		memcpy(lines, g_mcChat, sizeof(lines));
		g_mcChatCount = 0;
		ReleaseSRWLockExclusive(&g_mcChatLock);
		for (int i = 0; i < n; i++)
			kailleraChatSend(lines[i]);
	}

	// Sends card s and says how it went - true when WE Camp has `data`.
	static bool McSend(McSession* m, int s, const char* data) {
		char card[64];
		McCardName(m, s, card, sizeof(card));
		char bases[N02_MC_KNOWN_MAX * 65 + 1] = "";
		for (int i = 0; i < m->knownCount[s]; i++) {
			if (i)
				strcat(bases, ",");
			strcat(bases, m->known[s][i]);
		}
		char status[64];
		int version = 0;
		n02_wecamp_commit(m->contentId, m->owner[s], bases, m->players, m->iso[s], s + 1, m->nick,
			data, status, sizeof(status), &version);
		if (strcmp(status, "committed") == 0) {
			McLog("* %s salvo no WE Camp (versao %d).", card, version);
			char line[64];
			_snprintf(line, sizeof(line), "Memory Card Online Slot %d foi salvo.", s + 1);
			line[sizeof(line) - 1] = 0;
			McQueueChat(line);
			return true;
		}
		if (strcmp(status, "already") == 0)
			return true; // the host (or another player) already saved this exact card
		if (strcmp(status, "unchanged") == 0)
			return true;
		if (strcmp(status, "conflict") == 0)
			McLog(m->iso[s]
				? "* %s NAO foi salvo: ele mudou no WE Camp durante a partida "
				  "(outra partida com o mesmo cartao, envio pelo site ou cartao diferente do salvo por outro jogador)."
				: "* %s NAO foi salvo: ele mudou no WE Camp durante a partida "
				  "(envio pelo site ou cartao diferente do salvo por outro jogador).", card);
		else
			McLog("* %s NAO foi salvo (%s).", card, status);
		return false;
	}

	static DWORD WINAPI McWatchThread(LPVOID param) {
		McSession* m = (McSession*)param;
		char* now = (char*)malloc(5 * N02_MCD_SIZE);
		if (now == NULL)
			return 0;
		char* pending[2] = { now + N02_MCD_SIZE, now + 2 * N02_MCD_SIZE };   // last read
		char* settled[2] = { now + 3 * N02_MCD_SIZE, now + 4 * N02_MCD_SIZE }; // last finished save
		DWORD changedAt[2] = { 0, 0 };
		for (int s = 0; s < 2; s++) {
			memcpy(pending[s], m->onServer[s], N02_MCD_SIZE);
			memcpy(settled[s], m->onServer[s], N02_MCD_SIZE);
		}
		while (WaitForSingleObject(m->stop, N02_MC_WATCH_MS) == WAIT_TIMEOUT) {
			for (int s = 0; s < m->slots; s++) {
				if (!m->track[s] || !McReadCard(m->dir, s, now))
					continue;
				if (memcmp(now, pending[s], N02_MCD_SIZE) != 0) {
					memcpy(pending[s], now, N02_MCD_SIZE); // the game is (still) writing it
					changedAt[s] = GetTickCount();
					continue;
				}
				if (memcmp(pending[s], settled[s], N02_MCD_SIZE) == 0
					|| GetTickCount() - changedAt[s] < N02_MC_SAVE_SETTLE_MS)
					continue;
				// A finished save.
				memcpy(settled[s], pending[s], N02_MCD_SIZE);
				char sha[65];
				if (!n02_wecamp_sha256(settled[s], N02_MCD_SIZE, sha))
					continue;
				if (m->sender[s] && McSend(m, s, settled[s])) {
					memcpy(m->onServer[s], settled[s], N02_MCD_SIZE);
					m->saved[s] = true;
				}
				McKnownAdd(m, s, sha);
			}
		}
		free(now);
		return 0;
	}

	static int McPrepareLive(const char* contentId, const char* gameFile, const char* dir) {
		static char buf[N02_MCD_SIZE + 1];
		char err[300];
		// Each slot is what its player picked (CurrentGameMcSlots()): their own
		// card ("M. Card Online"), the game's card of that slot ("M. Card da
		// ISO") or none - a choice only counted there for a confirmed account.
		// A PC without a login asks with the DLL's key, so every PC loads the
		// same slots.
		int choice[2];
		CurrentGameMcSlots(choice);
		bool loggedIn = n02_wecamp_logged_in();

		char nick[32];
		active_mod.GetOwnerName(nick, sizeof(nick));
		if (loggedIn && _stricmp(nick, n02_wecamp_username()) != 0) {
			McLog("* Memory Card online: seu nick (%s) e diferente da sua conta WE Camp (%s).", nick, n02_wecamp_username());
			return -1;
		}

		// 1P, 2P, ... - the room list is in the server's join order (P2P: the
		// host, then the peer). Checked against our own player number, so a
		// list in a different order can never hand anybody the wrong card
		// silently.
		char names[8][32];
		int count = get_active_mode_index() == 0 ? p2p_GetGamePlayers(names, 8) : kaillera_GetGamePlayers(names, 8);
		if (numplayers < count)
			count = numplayers;
		if (playerno < 1 || playerno > count || _stricmp(names[playerno - 1], nick) != 0) {
			McLog("* Memory Card online: nao consegui confirmar a ordem dos jogadores.");
			return -1;
		}
		McSession* m = new McSession();
		for (int i = 0; i < count; i++) {
			if (i) strcat(m->players, ",");
			strcat(m->players, names[i]);
		}

		// One checkout per kind of card: "M. Card Online" for the players who
		// picked it (WE Camp hands the others' slots back empty), "M. Card da
		// ISO" for the game's cards.
		char online[2 * 32 + 2] = "";
		bool anyIso = false;
		for (int s = 0; s < 2; s++) {
			if (choice[s] == N02_MEMCARD_CHOICE_ONLINE && s < count) {
				if (online[0]) strcat(online, ",");
				strcat(online, names[s]);
			} else if (choice[s] == N02_MEMCARD_CHOICE_ISO) {
				anyIso = true;
			}
		}
		n02_wecamp_checkout_result co[2]; // [0] M. Card Online, [1] M. Card da ISO
		memset(co, 0, sizeof(co));
		if (online[0] && !n02_wecamp_checkout(contentId, gameFile, m->players, false, online, &co[0], err, sizeof(err))) {
			McLog("* Memory Card online indisponivel: %s", err);
			delete m;
			return -1;
		}
		if (anyIso && !n02_wecamp_checkout(contentId, gameFile, m->players, true, NULL, &co[1], err, sizeof(err))) {
			McLog("* M. Card da ISO indisponivel: %s", err);
			delete m;
			return -1;
		}
		if (loggedIn && !n02_wecamp_logged_in()) {
			// The site refused our login (the checkout went again with the
			// DLL's key): from here on, a PC without one.
			loggedIn = false;
			McLog("* A sua conexao com a conta WE Camp caiu: entre de novo pelo botao \"M. Card Online\" da tela inicial do Kaillera.");
		}
		bool multiBase = true;
		char sha[2][65] = { "", "" };
		int version[2] = { 0, 0 };
		for (int s = 0; s < 2; s++) {
			if (choice[s] == N02_MEMCARD_CHOICE_NONE)
				continue;
			bool iso = choice[s] == N02_MEMCARD_CHOICE_ISO;
			const n02_wecamp_checkout_result* c = &co[iso ? 1 : 0];
			if (s >= c->slots || c->sha256[s][0] == 0)
				continue; // no card there (a server from before the ISO's slot 2): the core makes a blank one, nothing is saved
			if (!n02_wecamp_get_card(c->sha256[s], buf, !loggedIn) || !McWriteCard(dir, s, buf)) {
				if (iso)
					McLog("* M. Card da ISO: falha ao baixar o cartao do slot %d.", s + 1);
				else
					McLog("* Memory Card online: falha ao baixar o cartao de %s.", c->player[s]);
				delete m;
				return -1;
			}
			multiBase = multiBase && c->multiBase;
			m->hasCard[s] = true;
			m->iso[s] = iso;
			memcpy(m->onServer[s], buf, N02_MCD_SIZE);
			McKnownAdd(m, s, c->sha256[s]);
			strncpy(m->owner[s], c->player[s], sizeof(m->owner[s]) - 1);
			strcpy(sha[s], c->sha256[s]);
			version[s] = c->version[s];
			// The slot's own player sends it first (a solo game's 1P its ISO
			// slot 2 too); sending an online card back takes a login, the
			// game's card goes with the DLL's key too.
			m->sender[s] = _stricmp(names[s < count ? s : 0], nick) == 0;
			m->track[s] = loggedIn || iso;
		}
		m->slots = 2;
		m->midMatch = multiBase;
		for (int s = 0; s < 2; s++)
			if (!m->hasCard[s])
				g_mc_empty_slots |= 1 << s; // "Sem M. Card" (or no card there): no card inserted, not a blank one
		strncpy(m->dir, dir, sizeof(m->dir) - 1);
		strncpy(m->contentId, contentId, sizeof(m->contentId) - 1);
		strncpy(m->nick, nick, sizeof(m->nick) - 1);

		McWriteMarker(sha[0], sha[1]);
		{
			char line[400] = "* Memory Card:";
			for (int s = 0; s < 2; s++) {
				size_t len = strlen(line);
				if (m->hasCard[s] && m->iso[s])
					_snprintf(line + len, sizeof(line) - len, "%s slot %d = cartao %d do jogo (M. Card da ISO, versao %d)",
						s ? "," : "", s + 1, s + 1, version[s]);
				else if (m->hasCard[s])
					_snprintf(line + len, sizeof(line) - len, "%s slot %d = cartao de %s (versao %d)", s ? "," : "", s + 1,
						m->owner[s], version[s]);
				else
					_snprintf(line + len, sizeof(line) - len, "%s slot %d = sem memory card", s ? "," : "", s + 1);
				line[sizeof(line) - 1] = 0;
			}
			McLog("%s.", line);
		}
		if (!loggedIn && online[0])
			McLog("* Voce nao esta logado no WE Camp: o dono de cada M. Card Online salva o seu.");

		// Without a card this PC may send, it has nothing more to do.
		if (!m->track[0] && !m->track[1]) {
			delete m;
			return 1;
		}
		McLog(m->midMatch ? "* Cada save no jogo vai para o WE Camp assim que o jogo termina de gravar."
			: "* Os saves vao para o WE Camp no fim da partida.");
		m->stop = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (m->midMatch && m->stop)
			m->watcher = CreateThread(NULL, 0, McWatchThread, m, 0, NULL);
		g_mcs = m;
		return 1;
	}

	int KAILLERA_DLLEXP kailleraGetMemcardMode(){
		return CurrentGameMemcardMode();
	}

	/* After kailleraMemcardPrepare() returned 1: the slots the match starts
	   with no memory card inserted (bit 0 = slot 1, bit 1 = slot 2) - the
	   frontend runs them on "none" instead of a blank card. */
	int KAILLERA_DLLEXP kailleraMemcardEmptySlots(){
		return g_mc_empty_slots;
	}

	void KAILLERA_DLLEXP kailleraMemcardFinish();

	int KAILLERA_DLLEXP kailleraMemcardPrepare(const char* contentId, const char* gameFile, const char* dir){
		if (g_mcs)
			kailleraMemcardFinish(); // the last match's never ended - send what it has
		g_mc_empty_slots = 0;
		if (contentId == NULL || dir == NULL || CurrentGameMemcardMode() != N02_MEMCARD_ONLINE)
			return 0;

		char sha[2][65];
		if (kaillera_retryconnect_active()) {
			kaillera_retryconnect_mconline(sha);
			return McPrepareFromMarker(dir, sha);
		}
		if (get_active_mode_index() == 2 || player_is_watching()) {
			player_get_mconline(sha);
			return McPrepareFromMarker(dir, sha);
		}
		if (get_active_mode_index() == 1 || get_active_mode_index() == 0)
			return McPrepareLive(contentId, gameFile ? gameFile : "", dir);
		return 0;
	}

	// Every PC ends the match with the same cards, so one send is enough: a
	// slot's sender sends it right away, everybody else only after a while -
	// by then the sender's is normally in and theirs is an "already"; if the
	// sender dropped, theirs is the one that saves the card.
	#define N02_MC_GUEST_COMMIT_DELAY_MS 15000

	static DWORD WINAPI McFinishThread(LPVOID param) {
		McSession* m = (McSession*)param;
		if (m->watcher) {
			WaitForSingleObject(m->watcher, INFINITE); // a save it may still be sending
			CloseHandle(m->watcher);
		}
		// First the slots this PC sends first, then - after the wait - the others.
		for (int pass = 0; pass < 2; pass++) {
			bool waited = false;
			for (int s = 0; s < m->slots; s++) {
				if (!m->track[s] || m->sender[s] != (pass == 0))
					continue; // empty slot, or one this PC can't send - nothing to save
				if (pass == 1 && !waited) {
					Sleep(N02_MC_GUEST_COMMIT_DELAY_MS);
					waited = true;
				}
				char card[64];
				McCardName(m, s, card, sizeof(card));
				if (!m->finalOk[s]) {
					McLog("* %s: nao encontrei o cartao para salvar.", card);
					continue;
				}
				// Nothing new since the match began, or since this PC's last send.
				if (memcmp(m->final[s], m->onServer[s], N02_MCD_SIZE) == 0) {
					if (m->sender[s] && !m->saved[s])
						McLog("* %s sem alteracoes.", card);
					continue;
				}
				McSend(m, s, m->final[s]);
			}
		}
		if (m->stop)
			CloseHandle(m->stop);
		delete m;
		return 0;
	}

	void KAILLERA_DLLEXP kailleraMemcardFinish(){
		McSession* m = g_mcs;
		g_mcs = NULL;
		if (m == NULL)
			return;
		// The cards as the match ended - read now, the next match reuses the folder.
		for (int s = 0; s < m->slots; s++)
			if (m->track[s])
				m->finalOk[s] = McReadCard(m->dir, s, m->final[s]);
		if (m->stop)
			SetEvent(m->stop);
		HANDLE t = CreateThread(NULL, 0, McFinishThread, m, 0, NULL);
		if (t) {
			CloseHandle(t);
			return;
		}
		// No thread for the sends: stop watching and drop the cards' end.
		if (m->watcher) {
			WaitForSingleObject(m->watcher, INFINITE);
			CloseHandle(m->watcher);
		}
		if (m->stop)
			CloseHandle(m->stop);
		delete m;
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
		// The "Ir ao vivo!" lock is enforced here, before anything is asked
		// of the host - see player_watch_golive_locked(). An older frontend
		// that doesn't check kailleraWatchGoLiveLocked() just gets "couldn't
		// request" instead of a state it would load and then be refused.
		if (player_watch_golive_locked())
			return 0;
		return n02_watch_request_state() ? 1 : 0;
	}
	/* Nonzero while "Ir ao vivo!" is locked (right after a jump, until the
	   spectator pauses or rewinds) - lets the frontend grey the button out
	   and explain why, instead of calling kailleraWatchRequestState(). */
	int KAILLERA_DLLEXP kailleraWatchGoLiveLocked(){
		return player_watch_golive_locked() ? 1 : 0;
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
	   kailleraPlaybackSeekToFrame() above does. Never refused: the state is
	   already loaded, so skipping the jump would leave it reading whatever
	   input this spectator had reached - a guaranteed desync. */
	void KAILLERA_DLLEXP kailleraWatchJumpToLive(int frameIndex, int byteOffset){
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
