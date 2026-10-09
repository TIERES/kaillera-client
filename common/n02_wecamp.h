#pragma once

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

// Logs in with the account's e-mail and password. Returns true and stores
// token+username+e-mail on success; otherwise false with a message for the
// user in err (Portuguese, no accents).
bool n02_wecamp_login(const char* email, const char* password, char* err, int errCap);
void n02_wecamp_logout();

// "So logados" rooms: a short single-use ticket proving this account, bound
// to the room (the Kaillera game id) - sent in the room's chat on joining.
bool n02_wecamp_get_ticket(const char* room, char* ticket, int ticketCap, char* err, int errCap);
// Host side: checks a joiner's ticket; on success username gets the
// account's name (to compare with the joiner's nick).
bool n02_wecamp_verify_ticket(const char* ticket, const char* room, char* username, int usernameCap, char* err, int errCap);

struct n02_wecamp_checkout_result {
	int slots;               // 1 or 2
	char player[2][32];      // card owners: slot 1 = 1P, slot 2 = 2P
	char sha256[2][65];      // their current versions
	int version[2];
};

// Start of an online-card match. players = nicks in 1P,2P,... order,
// comma separated. Returns true on success; on failure err has the reason
// (missing_account lists who has no confirmed account).
bool n02_wecamp_checkout(const char* contentId, const char* gameName, const char* players,
	n02_wecamp_checkout_result* out, char* err, int errCap);

// Downloads one card version (N02_MCD_SIZE bytes into buf, which must hold
// N02_MCD_SIZE + 1 - a terminating NUL is written after the data) - with the
// player's token, or with the DLL's shared API key for spectators/replays.
bool n02_wecamp_get_card(const char* sha256, char* buf, bool useApiKey);

// End of the match: the final content of one card. status receives the
// server's verdict ("committed", "pending", "unchanged", "conflict", ...).
bool n02_wecamp_commit(const char* contentId, const char* slotPlayer, const char* baseSha256,
	const char* players, const char* data, char* status, int statusCap, int* version);

// Discord voice channel of a match (wg-camp's /api/voice/join): the server
// creates (or reuses) a private voice channel for the room's players in the
// WE Camp Discord and moves THIS player into it when they're already in one
// of its voice channels. players = the room's nicks, comma separated.
// status: "moved" or "link" (not in voice - open url/appUrl to join);
// false with errCode "not_linked" (no Discord linked on the site),
// "disabled" (off on the server) or another reason in err.
bool n02_wecamp_voice_join(const char* players, char* status, int statusCap, char* url, int urlCap,
	char* appUrl, int appUrlCap, char* errCode, int errCodeCap, char* err, int errCap);
