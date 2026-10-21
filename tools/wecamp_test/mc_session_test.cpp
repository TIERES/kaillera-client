// Integration test of kailleraclient.cpp's M. Card session code (the real
// code, copied out of kailleraclient.cpp by extract_mc_region.py) against a local wg-camp: saves during the match,
// the end-of-match send, the guest fallback and the chat line.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../common/n02_wecamp.h"
#include "../../common/n02_mcchoice.h"
#include "../../common/nSettings.h"

#define KAILLERA_DLLEXP __stdcall
#define N02_MEMCARD_ONLINE 2
#define N02_MEMCARD_MARKER_NICK "n02"
#define N02_MCONLINE_MARKER_PREFIX "[M. Card online] "
#define N02_MCONLINE_MARKER_NOCARD "none"

HINSTANCE hx = NULL; // nSettings: n02.ini next to the exe

// ---- the room around the session code ----
static bool t_host = true;
static char t_nick[32] = "Pele";
int playerno = 1, numplayers = 1;
static char t_names[2][32] = { "Pele", "Convidado" };
static int t_count = 1;
static int t_slots[2] = { N02_MEMCARD_CHOICE_ISO, N02_MEMCARD_CHOICE_ISO }; // solo: the 1P's ISO fills both

static void CurrentGameMcSlots(int slot[2]) { slot[0] = t_slots[0]; slot[1] = t_slots[1]; }
static int CurrentGameMemcardMode() { return N02_MEMCARD_ONLINE; }
static int get_active_mode_index() { return 1; }
int p2p_GetGamePlayers(char out[][32], int max) { memcpy(out, t_names, sizeof(t_names)); return t_count; }
int kaillera_GetGamePlayers(char out[][32], int max) { memcpy(out, t_names, sizeof(t_names)); return t_count; }
static char t_room_game[128] = "ML (PCSX ReARMed)";
static void McRoomInfo(char* game, int gameCap, char* server, int serverCap) {
	strncpy(game, t_room_game, gameCap - 1);
	game[gameCap - 1] = 0;
	strncpy(server, "Teste", serverCap - 1);
	server[serverCap - 1] = 0;
}
bool kaillera_retryconnect_active() { return false; }
bool kaillera_retryconnect_mconline(char sha[2][65]) { return false; }
bool player_is_watching() { return false; }
bool player_get_mconline(char sha[2][65]) { return false; }
void n02_stream_push_chat(const char*, const char*) {}

static void McLog(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	printf("     LOG ");
	vprintf(fmt, args);
	printf("\n");
	va_end(args);
}
static char t_chat[16][128];
static int t_chatCount;
void KAILLERA_DLLEXP kailleraChatSend(char* text) {
	if (t_chatCount < 16)
		strcpy(t_chat[t_chatCount++], text);
	printf("     CHAT %s\n", text);
}
static void ownerName(char* out, int cap) { strncpy(out, t_nick, cap); }
static bool isHost() { return t_host; }
static bool noStream() { return false; }
static struct {
	void (*GetOwnerName)(char*, int);
	bool (*IsHost)();
	bool (*StreamingEnabled)();
} active_mod = { ownerName, isHost, noStream };

#include "mc_region_real.inc"

// ---- the test ----
static int failures;
#define CHECK(cond, what) do { if (cond) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); failures++; } } while (0)

static char g_dir[MAX_PATH];
static const char* kContent = "7a7a7a7a:1c473f60";

// The game saving: a few sectors of the card file, written the way the core does.
static void GameSaves(int slot, unsigned char value) {
	char path[MAX_PATH], card[N02_MCD_SIZE];
	_snprintf(path, sizeof(path), "%s\\pcsx-card%d.mcd", g_dir, slot);
	FILE* f = fopen(path, "rb");
	if (!f) { printf("     (sem %s)\n", path); return; }
	fread(card, 1, N02_MCD_SIZE, f);
	fclose(f);
	for (int sector = 0; sector < 8; sector++) {
		memset(card + 8192 + sector * 128, value + sector, 128);
		f = fopen(path, "r+b");
		fseek(f, 8192 + sector * 128, SEEK_SET);
		fwrite(card + 8192 + sector * 128, 1, 128, f);
		fclose(f);
		Sleep(150);
	}
}

static bool Versions(int* v1, int* v2) {
	n02_wecamp_checkout_result co;
	char err[300];
	if (!n02_wecamp_checkout(kContent, "ML.bin", "Pele,Convidado", true, NULL, &co, err, sizeof(err)))
		return false;
	*v1 = co.version[0];
	*v2 = co.slots > 1 ? co.version[1] : -1;
	return true;
}

// The current version of a player's own card of this game (slot 2 of a
// Pele,<name> checkout).
static int OnlineVersion(const char* name) {
	n02_wecamp_checkout_result co;
	char err[300], players[80];
	_snprintf(players, sizeof(players), "Pele,%s", name);
	if (!n02_wecamp_checkout(kContent, "ML.bin", players, false, name, &co, err, sizeof(err)))
		return -1;
	return co.slots > 1 ? co.version[1] : -1;
}

static bool ChatSaid(const char* text) {
	for (int i = 0; i < t_chatCount; i++)
		if (strcmp(t_chat[i], text) == 0)
			return true;
	return false;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	// Only ever against the local wg-camp: refuse to run if n02.ini doesn't point there.
	nSettings::Initialize((char*)"SC");
	char host[128];
	nSettings::get_str_in((char*)"SC", (char*)"WECAMP_HOST", host, (char*)"");
	if (strcmp(host, "127.0.0.1") != 0) {
		printf("n02.ini nao aponta para o wg-camp local (WECAMP_HOST=%s) - abortado\n", host);
		return 2;
	}
	n02_wecamp_load();
	n02_wecamp_logout();
	char err[300];
	GetTempPath(sizeof(g_dir), g_dir);
	strcat(g_dir, "mc_session_test");
	CreateDirectory(g_dir, NULL);

	CHECK(n02_wecamp_login("pele@example.com", "segredo123", err, sizeof(err)), "host logado");
	int v1, v2;
	CHECK(Versions(&v1, &v2) && v1 == 1 && v2 == 1, "jogo novo: cartoes formatados nos dois slots (v1)");

	// --- solo game, M. Card da ISO in both slots (the 1P's): saves during the match ---
	CHECK(kailleraMemcardPrepare(kContent, "ML.bin", g_dir) == 1 && g_mcs != NULL, "host: inicio da partida");
	CHECK(g_mcs && g_mcs->midMatch && g_mcs->watcher && g_mcs->sender[0] && g_mcs->sender[1]
		&& g_mcs->iso[0] && g_mcs->iso[1], "host: envia os dois slots durante a partida");
	CHECK(kailleraMemcardEmptySlots() == 0, "solo ISO: nenhum slot vazio");
	{
		// --- one match at a time with the game's cards ---
		char msg[300];
		n02_wecamp_checkout_result other;
		CHECK(g_mcs && g_mcs->isoLock, "trava: a partida prende o M. Card da ISO");
		// (with our token we must be in its list - another match of Pele's, with B)
		CHECK(!n02_wecamp_checkout(kContent, "ML.bin", "Pele,B", true, NULL, &other, err, sizeof(err), "Outra sala", "SP")
			&& other.busy && strstr(err, "Pele") != NULL, "trava: o checkout de outra partida e recusado (diz quem esta jogando)");
		CHECK(n02_wecamp_iso_lock("status", NULL, "A,B", t_room_game, "SP", msg, sizeof(msg)) == 0
			&& strstr(msg, "Teste") != NULL, "trava: o Start de outra sala do mesmo jogo fica bloqueado");
		CHECK(n02_wecamp_iso_lock("status", NULL, "Pele", t_room_game, "Teste", msg, sizeof(msg)) == 1,
			"trava: a propria partida nao se bloqueia");
		CHECK(n02_wecamp_iso_lock("status", NULL, "A,B", "Outro jogo", "SP", msg, sizeof(msg)) == 1,
			"trava: outro jogo nao e afetado");
	}
	GameSaves(2, 0x20);
	Sleep(2000);
	CHECK(Versions(&v1, &v2) && v2 == 1, "save ainda gravando/acomodando: nada enviado");
	Sleep(4000);
	CHECK(Versions(&v1, &v2) && v1 == 1 && v2 == 2, "save do slot 2 enviado durante a partida (v2)");
	CHECK(t_chatCount == 0, "o chat espera a thread do jogo");
	McFlushChat();
	CHECK(ChatSaid("Memory Card Online Slot 2 foi salvo."), "chat: Memory Card Online Slot 2 foi salvo.");
	GameSaves(1, 0x40);
	Sleep(6000);
	CHECK(Versions(&v1, &v2) && v1 == 2 && v2 == 2, "save do slot 1 enviado durante a partida (v2)");
	GameSaves(1, 0x50);       // a save the match ends in the middle of
	kailleraMemcardFinish();
	Sleep(4000);
	CHECK(Versions(&v1, &v2) && v1 == 3 && v2 == 2, "fim da partida: o ultimo save do slot 1 vai (v3), slot 2 sem nada novo");
	t_chatCount = 0;
	McFlushChat();
	CHECK(ChatSaid("Memory Card Online Slot 1 foi salvo."), "chat: Memory Card Online Slot 1 foi salvo.");
	{
		char msg[300];
		CHECK(n02_wecamp_iso_lock("status", NULL, "A,B", t_room_game, "SP", msg, sizeof(msg)) == 1,
			"trava: solta no fim da partida - a outra sala ja pode comecar");
	}

	// --- a match with no save sends nothing ---
	CHECK(kailleraMemcardPrepare(kContent, "ML.bin", g_dir) == 1, "partida sem save: inicio");
	Sleep(2000);
	kailleraMemcardFinish();
	Sleep(3000);
	CHECK(Versions(&v1, &v2) && v1 == 3 && v2 == 2, "partida sem save: nenhum envio");

	// --- each player's own slot: 1P (Pele, us) on M. Card da ISO, 2P (Mtgamess) on M. Card Online ---
	strcpy(t_names[1], "Mtgamess");
	t_count = numplayers = 2;
	t_slots[0] = N02_MEMCARD_CHOICE_ISO;
	t_slots[1] = N02_MEMCARD_CHOICE_ONLINE;
	int o2 = OnlineVersion("Mtgamess");
	CHECK(kailleraMemcardPrepare(kContent, "ML.bin", g_dir) == 1 && g_mcs != NULL, "misto: inicio da partida");
	CHECK(g_mcs && g_mcs->hasCard[0] && g_mcs->hasCard[1] && g_mcs->iso[0] && !g_mcs->iso[1]
		&& strcmp(g_mcs->owner[1], "Mtgamess") == 0 && kailleraMemcardEmptySlots() == 0,
		"misto: slot 1 = cartao 1 do jogo, slot 2 = cartao do Mtgamess");
	CHECK(g_mcs && g_mcs->sender[0] && !g_mcs->sender[1] && g_mcs->track[0] && g_mcs->track[1],
		"misto: envia o slot 1 primeiro, o slot 2 e do Mtgamess");
	GameSaves(2, 0x30);
	Sleep(6000);
	CHECK(OnlineVersion("Mtgamess") == o2 && g_mcs && g_mcs->knownCount[1] == 2,
		"misto: o save do slot 2 so e anotado (quem envia e o Mtgamess)");
	GameSaves(1, 0x35);
	Sleep(6000);
	CHECK(Versions(&v1, &v2) && v1 == 4 && v2 == 2, "misto: save do slot 1 (ISO) enviado durante a partida (v4)");
	kailleraMemcardFinish();
	Sleep(4000);
	CHECK(OnlineVersion("Mtgamess") == o2, "misto: o slot 2 espera 15 s depois da partida");
	Sleep(13000);
	CHECK(OnlineVersion("Mtgamess") == o2 + 1, "misto: envia o cartao do Mtgamess (ele caiu)");
	CHECK(Versions(&v1, &v2) && v1 == 4 && v2 == 2, "misto: os cartoes do jogo nao mudaram no fim");

	// --- guest (not logged in), 1P's slot 1 on M. Card da ISO: saves noted, sent 15 s after the match ---
	strcpy(t_names[1], "Convidado");
	t_slots[1] = N02_MEMCARD_CHOICE_NONE;
	n02_wecamp_logout();
	t_host = false;
	strcpy(t_nick, "Convidado");
	playerno = 2;
	CHECK(kailleraMemcardPrepare(kContent, "ML.bin", g_dir) == 1 && g_mcs && !g_mcs->sender[0] && g_mcs->track[0]
		&& !g_mcs->hasCard[1] && g_mcs->watcher, "convidado: inicio, so anota os saves");
	CHECK(kailleraMemcardEmptySlots() == 2, "convidado sem login: slot 2 sem cartao inserido");
	{
		// A replay of it: "none" = no card inserted; "-" (an older replay) = the blank card it had.
		char sha[2][65] = { "", N02_MCONLINE_MARKER_NOCARD };
		strcpy(sha[0], g_mcs->known[0][0]);
		McSession* live = g_mcs;
		g_mcs = NULL;
		g_mc_empty_slots = 0;
		CHECK(McPrepareFromMarker(g_dir, sha) == 1 && g_mc_empty_slots == 2, "replay: \"none\" = slot sem cartao");
		strcpy(sha[1], "-");
		g_mc_empty_slots = 0;
		CHECK(McPrepareFromMarker(g_dir, sha) == 1 && g_mc_empty_slots == 0, "replay antigo: \"-\" = cartao em branco");
		g_mcs = live;
	}
	GameSaves(1, 0x60);
	Sleep(6000);
	CHECK(Versions(&v1, &v2) && v1 == 4, "convidado: nao envia durante a partida");
	CHECK(g_mcs && g_mcs->knownCount[0] == 2, "convidado: anotou o save como versao vista");
	GameSaves(1, 0x70);       // the host dropped after this one - nobody sent it
	kailleraMemcardFinish();
	Sleep(5000);
	CHECK(Versions(&v1, &v2) && v1 == 4, "convidado: espera 15 s depois da partida");
	Sleep(13000);
	CHECK(Versions(&v1, &v2) && v1 == 5, "convidado: envia o cartao final (o host caiu)");

	printf(failures ? "\n%d FALHA(S)\n" : "\nTUDO OK\n", failures);
	return failures ? 1 : 0;
}
