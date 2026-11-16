#pragma once

// ISO check in the room, before Start (Walter, 2026-11-16). warleyvieira x
// luiz played "Euro vs Copa America 2004.bin" with two different files (same
// size, other bytes): the match itself went on alike, but each PC's RAM
// differed from the first second, the other player's online memory card came
// blank (cards go by the ISO's id) and WE Camp's "Ao vivo" got two half
// matches it never shows. RetroArch's in-game warning had lasted 15 s, during
// the PS1's boot.
//
// So, in the room: RetroArch TIERES gives the DLL the "CRC32:SIZE" of the
// room's game in this PC - the id its anti-desync fingerprint and the placar
// ao vivo use (kailleraSetContentIdCallback) - and every player's DLL
// announces its own in the room's chat, on entering and to everyone who
// enters later, and keeps everybody's in a n02_isoids table. With different
// ids: a red chat line on every PC as soon as it is known, and at the host's
// Start a popup on every PC (N02_ISO_WARN) - the host's asks whether to start
// anyway. With 3+ players, whoever is outside the biggest group is named as
// the one with the different ISO; with 2 (or a tie) nobody can tell which one
// is right. An older DLL, another emulator or a game this PC can't read never
// announces an id and isn't compared. Same ISOs: nothing is shown.

// "[ISO] 73E91CFE:1C473F60" (a player's id) / "[ISO] aviso" (the host pressed
// Start with different ISOs: every PC shows its popup).
#define N02_ISO_CHAT_PREFIX "[ISO] "
#define N02_ISO_WARN "aviso"

// RetroArch's kailleraContentIdForGame() (kailleraclient.cpp's
// kailleraSetContentIdCallback export), NULL with any other emulator. Called
// from the room window's thread, never blocks: start = 1 begins hashing the
// game's ISO, then 1 = `out` has its id, 0 = still hashing (ask again later),
// -1 = not in this PC's list / unreadable.
typedef int (__stdcall *n02_content_id_fn)(const char* game, int start, char* out, int cap);
extern n02_content_id_fn n02_content_id_cb;

#define N02_ISOIDS_MAX 16
#define N02_ISOID_LEN 24
struct n02_isoids {
	char nick[N02_ISOIDS_MAX][32];
	char id[N02_ISOIDS_MAX][N02_ISOID_LEN];
	int count;
};

void n02_isoids_clear(n02_isoids* t);
// True when it's new or changed.
bool n02_isoids_set(n02_isoids* t, const char* nick, const char* id);
void n02_isoids_remove(n02_isoids* t, const char* nick);
// NULL: never announced.
const char* n02_isoids_get(const n02_isoids* t, const char* nick);

// "XXXXXXXX:Y..." - 8 hex digits, ':', the size in hex (what RetroArch's
// fingerprint sends; the chat line's text after the prefix).
bool n02_isoid_valid(const char* text);

// The room's verdict for the players in `names` (the room's list, 1P, 2P...).
// Groups go in the order of their first player in the list, so every PC of
// the room writes the same text.
#define N02_ISO_GROUPS_MAX 8
#define N02_ISO_NICKS_LEN (N02_ISOIDS_MAX * 36)
struct n02_isoverdict {
	int groups;   // distinct ids among the players who announced one
	int unknown;  // players who never announced one (not compared)
	int majority; // the strictly biggest group, -1: none (a tie - 2 players)
	char id[N02_ISO_GROUPS_MAX][N02_ISOID_LEN];
	int size[N02_ISO_GROUPS_MAX];
	char nicks[N02_ISO_GROUPS_MAX][N02_ISO_NICKS_LEN]; // "a, b e c"
	// With a majority: everybody outside it; without: nobody (see the groups).
	char odd[N02_ISO_NICKS_LEN];
	int odd_count;
	// Each player who announced one, in the list's order, and the group.
	int players;
	char player[N02_ISOIDS_MAX][32];
	int player_group[N02_ISOIDS_MAX];
};
// True when the ISOs differ (2+ groups).
bool n02_iso_verdict(const n02_isoids* t, const char names[][32], int count, n02_isoverdict* v);

// The verdict as text ("" when they don't differ) - to tell a change apart.
void n02_iso_signature(const n02_isoverdict* v, char* out, int cap);

// The red line in the room's chat (no accents: the chat is ANSI).
void n02_iso_chat_line(const n02_isoverdict* v, char* out, int cap);

// The popup's text for the player `me` of the room playing `game`; for the
// host it ends asking whether to start anyway.
void n02_iso_popup_text(const n02_isoverdict* v, const char* me, const char* game, bool host,
	wchar_t* out, int cap);
#define N02_ISO_POPUP_TITLE L"Kaillera - ISO diferente"
