#pragma once

// Memory card of a live match (Server-mode room or P2P connection), chosen
// by each player for their own slot (Walter, 2026-10-20): the 1P's choice
// fills slot 1 and the 2P's slot 2 - "Sem M. Card" (empty slot), "M. Card
// Online" (that player's own card on WE Camp) or "M. Card da ISO" (the game's
// own WE Camp card of that slot, the same for everyone who plays it). From
// the 3P on there's no slot left, so their choice doesn't count. In a solo
// game the 1P's "M. Card da ISO" fills slot 2 too (the game's two cards, as
// before). Only a player logged in to WE Camp chooses - whoever isn't is
// "Sem M. Card" - and only a confirmed account (the host's ticket check)
// counts at game start. PS1 (PCSX ReARMed) games only: any other game plays
// without, whatever was chosen.
//
// Each player's DLL announces its own choice in the room's chat - on
// entering, when somebody else enters (so they learn it) and on every change
// - and every DLL keeps everybody's in a n02_mcchoices table, from which all
// PCs work out the same slots at game start. An older DLL never announces
// one and plays "Sem M. Card", so a game with a player who never did goes
// without memory card for everybody.

#define N02_MEMCARD_CHOICE_NONE   0 // the list's order too
#define N02_MEMCARD_CHOICE_ONLINE 1
#define N02_MEMCARD_CHOICE_ISO    2

// "[M. Card] online" / "[M. Card] iso" / "[M. Card] sem" / "[M. Card] sem login"
#define N02_MCCHOICE_CHAT_PREFIX "[M. Card] "

#define N02_MCCHOICES_MAX 16
struct n02_mcchoices {
	char nick[N02_MCCHOICES_MAX][32];
	int choice[N02_MCCHOICES_MAX];
	bool noLogin[N02_MCCHOICES_MAX];
	int count;
};

void n02_mcchoices_clear(n02_mcchoices* t);
// True when it's new or changed.
bool n02_mcchoices_set(n02_mcchoices* t, const char* nick, int choice, bool noLogin);
void n02_mcchoices_remove(n02_mcchoices* t, const char* nick);
// -1: never announced.
int n02_mcchoices_get(const n02_mcchoices* t, const char* nick);
bool n02_mcchoices_no_login(const n02_mcchoices* t, const char* nick);

// The chat line for a choice (out gets the whole line) and back - false when
// the text after the prefix isn't one.
void n02_mcchoice_line(int choice, bool noLogin, char* out, int cap);
bool n02_mcchoice_parse(const char* text, int* choice, bool* noLogin);
// "Sem M. Card" / "M. Card Online" / "M. Card da ISO"
const char* n02_mcchoice_label(int choice);

// The game is a PCSX ReARMed one (the room's / connection's game name).
bool n02_mcchoice_is_pcsx(const char* game);

// The slots of the game starting now: names = its players in 1P, 2P, ...
// order, accounts = the confirmed WE Camp accounts, comma separated (NULL:
// none known). slot[s] = N02_MEMCARD_CHOICE_*; *outdated = a player never
// announced a choice (an older DLL) - then both are "Sem M. Card".
void n02_mcchoice_game_slots(const char names[][32], int count, const n02_mcchoices* t,
	const char* accounts, bool pcsx, int slot[2], bool* outdated);
