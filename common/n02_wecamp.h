#pragma once

#include <windows.h>

// WE Camp account + Memory Card online client (wg-camp's /api/mc/*).
//
// Unlike the other community-server helpers (n02_stream/n02_watch/
// n02_replays, plain HTTP on :8080), this one speaks HTTPS through WinHTTP:
// it carries the player's password (once, at login) and the API token that
// identifies them afterwards. Responses are "key=value" lines.
//
// The token and the account's username are kept in n02.ini (WECAMP_TOKEN /
// WECAMP_USER, section of whoever called nSettings::Initialize) - the
// password never is. Changing the password on the site revokes the token.
//
// All calls block (timeouts of a few seconds) - call them from a worker
// thread or from places where a short wait is fine (the game-start hook).

#define N02_WECAMP_HOST "we2002.wgs.dev.br"
#define N02_WECAMP_PORT 443
// Site pages: sign up, and recover username / reset password.
#define N02_WECAMP_SIGNUP_URL "https://we2002.wgs.dev.br/conta/cadastro"
#define N02_WECAMP_FORGOT_URL "https://we2002.wgs.dev.br/conta/recuperar"
#define N02_MCD_SIZE (128 * 1024)

// Logged-in account (from n02.ini) - empty username = not logged in. The
// username is the account's fixed name, which is also the player's Kaillera
// nick while logged in.
void n02_wecamp_load();
const char* n02_wecamp_username();
const char* n02_wecamp_email();
bool n02_wecamp_logged_in();

// Discord linked on the site (cached in n02.ini, refreshed at login and by
// n02_wecamp_refresh_discord(), which blocks - call it from a worker
// thread) and the "Chamada de voz no Discord" choice of the M. Card Online
// dialog (DISCORD_VOICE): off by default, switched on by itself once per
// account when its Discord first shows up linked; the player can turn it
// off. n02_wecamp_voice_enabled() = linked and switched on.
bool n02_wecamp_discord_linked();
const char* n02_wecamp_discord_name();
bool n02_wecamp_voice_enabled();
void n02_wecamp_set_voice_enabled(bool on);
bool n02_wecamp_refresh_discord();
// Same, on a worker thread; posts msg to notify (if any) when the site
// answered - wParam 1: it refused the token and the login is gone.
void n02_wecamp_refresh_discord_async(HWND notify, UINT msg);

// Any call whose token the site refuses (HTTP 401: "Desconectar" in another
// RetroArch or PC with this same n02.ini, password changed, connection
// removed on the site) forgets the login - n02_wecamp_logged_in() turns
// false - and the calls the DLL's key can make go again with it. What the
// player is told when that's noticed on opening the window:
#define N02_WECAMP_REFUSED_NOTICE "Sua conta WE Camp nao esta mais conectada neste PC: o site recusou a conexao " \
	"salva (\"Desconectar\" em outro RetroArch ou PC que usava a mesma, troca de senha ou conexao removida no site).\n\n" \
	"Entre de novo pelo botao \"M. Card Online\"."

// Logs in with the account's e-mail and password. Returns true and stores
// token+username+e-mail on success; otherwise false with a message for the
// user in err (Portuguese, no accents).
bool n02_wecamp_login(const char* email, const char* password, char* err, int errCap);
void n02_wecamp_logout();

// "So logados" rooms: a short single-use ticket proving this account, bound
// to the room (the Kaillera game id) - sent in the room's chat on joining.
bool n02_wecamp_get_ticket(const char* room, char* ticket, int ticketCap, char* err, int errCap);
// Host side (logged in or not - the API key then): checks a joiner's ticket; on success username gets the
// account's name (to compare with the joiner's nick).
bool n02_wecamp_verify_ticket(const char* ticket, const char* room, char* username, int usernameCap, char* err, int errCap);

struct n02_wecamp_checkout_result {
	int slots;               // 1 or 2
	char player[2][32];      // card owners: slot 1 = 1P, slot 2 = 2P
	char sha256[2][65];      // their current versions
	int version[2];
	bool multiBase;          // commits take a list of bases: saves can go out during the match
	bool locked;             // "M. Card da ISO": this match holds the game's cards (n02_wecamp_iso_lock renew/release)
	bool busy;               // ... refused: another match holds them (err = who)
};

// Start of an online-card match. players = nicks in 1P,2P,... order,
// comma separated. Returns true on success; on failure err has the reason
// (missing_account lists who has no confirmed account).
// iso = "M. Card da ISO": the game's own cards (no owner, the same for
// everyone), one for slot 1 and one for slot 2 - a server from before slot 2
// existed sends slot 1 only (slots = 1); a player who isn't logged in asks
// with the DLL's shared API key.
// accounts (M. Card Online; NULL = the old rule, both players need an
// account): nicks whose WE Camp account the room confirmed, comma separated.
// A player not in it gets an empty slot (player[s] and sha256[s] empty) -
// at least one slot has a card. Asked with the API key when not logged in,
// so every PC gets the same slots.
// lockRoomGame (iso only; NULL = no lock, like before): one match at a time
// with the game's cards - the checkout takes them for this match (the same
// players list on every PC of it) or fails with out->busy and who has them
// in err. lockRoomGame = the room's game name (what n02_wecamp_iso_lock's
// "status" looks for before the Start), lockServer = where the room is.
bool n02_wecamp_checkout(const char* contentId, const char* gameName, const char* players, bool iso,
	const char* accounts, n02_wecamp_checkout_result* out, char* err, int errCap,
	const char* lockRoomGame = NULL, const char* lockServer = NULL);

// The "M. Card da ISO" lock (wg-camp's /api/mc/iso-lock), with the token or
// the DLL's key. action:
//  "status"  - before the Start: does another match of the game named
//              roomGame hold the cards? (contentId unused)
//  "renew"   - during the match, every minute (the lock lapses without it)
//  "release" - end of the match
// 1 = free / done, 0 = another match holds them (msg = who, for the room),
// -1 = no answer, or a server without the lock.
int n02_wecamp_iso_lock(const char* action, const char* contentId, const char* players,
	const char* roomGame, const char* server, char* msg, int msgCap);

// Downloads one card version (N02_MCD_SIZE bytes into buf, which must hold
// N02_MCD_SIZE + 1 - a terminating NUL is written after the data) - with the
// player's token, or with the DLL's shared API key for spectators/replays.
bool n02_wecamp_get_card(const char* sha256, char* buf, bool useApiKey);

// One card's content - at the end of the match, or a save during it.
// baseSha256: the version the match began with or, with multiBase, a comma
// separated list of it and each save this PC saw (WE Camp takes the send while
// its current version is any of them). status receives the server's verdict
// ("committed", "already", "unchanged", "conflict", ...).
// iso: one of the game's cards, isoSlot 1 or 2 (slotPlayer unused); reporter =
// our nick, which the server notes when we send it with the API key (not
// logged in).
// SHA-256 of data as 64 lowercase hex digits - the id WE Camp gives a card version.
bool n02_wecamp_sha256(const void* data, int len, char out[65]);

bool n02_wecamp_commit(const char* contentId, const char* slotPlayer, const char* baseSha256,
	const char* players, bool iso, int isoSlot, const char* reporter, const char* data, char* status, int statusCap,
	int* version);

// Discord voice channel of a match (wg-camp's /api/voice/join): the server
// creates (or reuses) a private voice channel for the room's players in the
// WE Camp Discord and moves THIS player into it when they're already in one
// of its voice channels. players = the room's nicks, comma separated.
// status: "moved" or "link" (not in voice - open url/appUrl to join);
// false with errCode "not_linked" (no Discord linked on the site),
// "disabled" (off on the server) or another reason in err.
bool n02_wecamp_voice_join(const char* players, char* status, int statusCap, char* url, int urlCap,
	char* appUrl, int appUrlCap, char* errCode, int errCodeCap, char* err, int errCap);
