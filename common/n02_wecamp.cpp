#include "n02_wecamp.h"
#include "n02_stream.h" // N02_STREAM_DEFAULT_API_KEY - the DLL's shared key
#include "nSettings.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_token[128];
static char g_username[32];
static char g_email[128];
// Discord linked to the account on the site (cached from login / whoami)
// and the player's "Chamada de voz no Discord" choice (DISCORD_VOICE).
static bool g_discord_linked = false;
static char g_discord_name[128];
static bool g_voice = false;
// Server - always the production site, except when n02.ini overrides it for
// testing against a local wg-camp (WECAMP_HOST / WECAMP_PORT, WECAMP_HTTP=1
// for plain HTTP; see tools/wecamp_test).
static wchar_t g_host[128] = L"" N02_WECAMP_HOST;
static int g_port = N02_WECAMP_PORT;
static bool g_secure = true;

// The account lives in the Server mode's section of n02.ini ("SC", where it
// always was) whichever mode reads it - P2P uses the same login.
#define N02_WECAMP_SECTION ((char*)"SC")

void n02_wecamp_load() {
	char host[128];
	nSettings::get_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_HOST", host, (char*)N02_WECAMP_HOST);
	host[sizeof(host) - 1] = 0;
	MultiByteToWideChar(CP_UTF8, 0, host, -1, g_host, 128);
	g_port = nSettings::get_int_in(N02_WECAMP_SECTION, (char*)"WECAMP_PORT", N02_WECAMP_PORT);
	g_secure = nSettings::get_int_in(N02_WECAMP_SECTION, (char*)"WECAMP_HTTP", 0) == 0;

	nSettings::get_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_TOKEN", g_token, (char*)"");
	nSettings::get_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_USER", g_username, (char*)"");
	nSettings::get_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_EMAIL", g_email, (char*)"");
	g_token[sizeof(g_token) - 1] = 0;
	g_username[sizeof(g_username) - 1] = 0;
	g_email[sizeof(g_email) - 1] = 0;
	if (g_token[0] == 0) {
		g_username[0] = 0;
		g_email[0] = 0;
	}
	g_discord_linked = g_token[0] && nSettings::get_int_in(N02_WECAMP_SECTION, (char*)"DISCORD_LINKED", 0) != 0;
	nSettings::get_str_in(N02_WECAMP_SECTION, (char*)"DISCORD_NAME", g_discord_name, (char*)"");
	g_discord_name[sizeof(g_discord_name) - 1] = 0;
	g_voice = nSettings::get_int_in(N02_WECAMP_SECTION, (char*)"DISCORD_VOICE", 0) != 0;
}

const char* n02_wecamp_username() {
	return g_username;
}

const char* n02_wecamp_email() {
	return g_email;
}

bool n02_wecamp_logged_in() {
	return g_token[0] != 0 && g_username[0] != 0;
}

///////////////////////////////////////////////////////////////////////////////
// HTTPS request through WinHTTP. body may be NULL. Response body (up to
// respCap-1 bytes, NUL-terminated when it's text) goes to resp; returns the
// HTTP status, or 0 when the server couldn't be reached.
///////////////////////////////////////////////////////////////////////////////

static int HttpsRequest(const char* method, const char* path, const char* extraHeaders,
	const char* body, int bodyLen, char* resp, int respCap, int* respLen) {
	if (respLen) *respLen = 0;
	if (resp && respCap > 0) resp[0] = 0;

	wchar_t wMethod[16], wPath[1024], wHeaders[512];
	MultiByteToWideChar(CP_UTF8, 0, method, -1, wMethod, 16);
	MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, 1024);
	MultiByteToWideChar(CP_UTF8, 0, extraHeaders ? extraHeaders : "", -1, wHeaders, 512);

	int status = 0;
	HINTERNET session = WinHttpOpen(L"n02-kailleraclient", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
		return 0;
	WinHttpSetTimeouts(session, 5000, 5000, 8000, 10000);
	HINTERNET connect = WinHttpConnect(session, g_host, (INTERNET_PORT)g_port, 0);
	HINTERNET request = connect ? WinHttpOpenRequest(connect, wMethod, wPath, NULL,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, g_secure ? WINHTTP_FLAG_SECURE : 0) : NULL;
	if (request
		&& WinHttpSendRequest(request, wHeaders[0] ? wHeaders : WINHTTP_NO_ADDITIONAL_HEADERS,
			wHeaders[0] ? (DWORD)-1L : 0, (LPVOID)body, (DWORD)bodyLen, (DWORD)bodyLen, 0)
		&& WinHttpReceiveResponse(request, NULL)) {
		DWORD code = 0, size = sizeof(code);
		if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX))
			status = (int)code;
		int total = 0;
		for (;;) {
			DWORD avail = 0, got = 0;
			if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
				break;
			char chunk[8192];
			if (avail > sizeof(chunk)) avail = sizeof(chunk);
			if (!WinHttpReadData(request, chunk, avail, &got) || got == 0)
				break;
			if (resp && total < respCap - 1) {
				int n = (int)got;
				if (n > respCap - 1 - total) n = respCap - 1 - total;
				memcpy(resp + total, chunk, n);
				total += n;
			}
		}
		if (resp && respCap > 0) resp[total] = 0;
		if (respLen) *respLen = total;
	}
	if (request) WinHttpCloseHandle(request);
	if (connect) WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);
	return status;
}

// Value of "key=..." in a key=value response (copied into out), or false.
static bool KvGet(const char* text, const char* key, char* out, int cap) {
	size_t keyLen = strlen(key);
	const char* line = text;
	while (line && *line) {
		const char* end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		if (len > keyLen && strncmp(line, key, keyLen) == 0 && line[keyLen] == '=') {
			size_t n = len - keyLen - 1;
			if ((int)n >= cap) n = cap - 1;
			memcpy(out, line + keyLen + 1, n);
			out[n] = 0;
			if (n > 0 && out[n - 1] == '\r') out[n - 1] = 0;
			return true;
		}
		line = end ? end + 1 : NULL;
	}
	if (cap > 0) out[0] = 0;
	return false;
}

// application/x-www-form-urlencoded / query-string escaping of one value.
static void UrlEncode(const char* in, char* out, int cap) {
	static const char hex[] = "0123456789ABCDEF";
	int o = 0;
	for (const unsigned char* p = (const unsigned char*)in; *p && o < cap - 4; p++) {
		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')
			|| *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			out[o++] = (char)*p;
		} else {
			out[o++] = '%';
			out[o++] = hex[*p >> 4];
			out[o++] = hex[*p & 15];
		}
	}
	out[o] = 0;
}

static void AuthHeader(char* out, int cap, const char* more) {
	_snprintf(out, cap, "Authorization: Bearer %s\r\n%s", g_token, more ? more : "");
	out[cap - 1] = 0;
}

static void ErrorFrom(int status, const char* resp, char* err, int errCap) {
	if (err == NULL || errCap <= 0)
		return;
	if (status == 0) {
		_snprintf(err, errCap, "Sem conexao com o WE Camp (%s).", N02_WECAMP_HOST);
	} else {
		char message[256];
		KvGet(resp, "message", message, sizeof(message));
		if (message[0])
			_snprintf(err, errCap, "%s", message);
		else
			_snprintf(err, errCap, "Erro %d do WE Camp.", status);
	}
	err[errCap - 1] = 0;
}

// The site's discord_linked / discord_name (login and whoami replies; an
// older server sends neither - nothing changes then). The voice call is off
// by default and switched on by itself once per account, the first time its
// Discord shows up linked - after that it's the player's choice.
// DISCORD_VOICE_AUTO = the account that choice belongs to.
static void ApplyDiscord(const char* resp) {
	char value[16], name[256];
	if (!KvGet(resp, "discord_linked", value, sizeof(value)))
		return;
	g_discord_linked = atoi(value) != 0;
	KvGet(resp, "discord_name", name, sizeof(name));
	wchar_t wide[128];
	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 128) == 0)
		wide[0] = 0;
	if (WideCharToMultiByte(CP_ACP, 0, wide, -1, g_discord_name, sizeof(g_discord_name), "?", NULL) == 0)
		g_discord_name[0] = 0;
	nSettings::set_int_in(N02_WECAMP_SECTION, (char*)"DISCORD_LINKED", g_discord_linked ? 1 : 0);
	nSettings::set_str_in(N02_WECAMP_SECTION, (char*)"DISCORD_NAME", g_discord_name);

	char owner[128];
	nSettings::get_str_in(N02_WECAMP_SECTION, (char*)"DISCORD_VOICE_AUTO", owner, (char*)"");
	owner[sizeof(owner) - 1] = 0;
	if (_stricmp(owner, g_username) != 0) {
		// Another account's choice (or none yet): start from this one's link.
		n02_wecamp_set_voice_enabled(g_discord_linked);
		if (g_discord_linked)
			nSettings::set_str_in(N02_WECAMP_SECTION, (char*)"DISCORD_VOICE_AUTO", g_username);
	}
}

bool n02_wecamp_discord_linked() {
	return n02_wecamp_logged_in() && g_discord_linked;
}

const char* n02_wecamp_discord_name() {
	return g_discord_name;
}

bool n02_wecamp_voice_enabled() {
	return n02_wecamp_discord_linked() && g_voice;
}

void n02_wecamp_set_voice_enabled(bool on) {
	g_voice = on;
	nSettings::set_int_in(N02_WECAMP_SECTION, (char*)"DISCORD_VOICE", on ? 1 : 0);
}

bool n02_wecamp_refresh_discord() {
	if (!n02_wecamp_logged_in())
		return false;
	char headers[256], resp[1024];
	AuthHeader(headers, sizeof(headers), NULL);
	if (HttpsRequest("GET", "/api/mc/whoami", headers, NULL, 0, resp, sizeof(resp), NULL) != 200)
		return false;
	ApplyDiscord(resp);
	return true;
}

bool n02_wecamp_login(const char* email, const char* password, char* err, int errCap) {
	char u[256], p[512], body[900], resp[1024];
	if (strchr(email, '@') == NULL) {
		_snprintf(err, errCap, "Informe o e-mail cadastrado no WE Camp.");
		err[errCap - 1] = 0;
		return false;
	}
	UrlEncode(email, u, sizeof(u));
	UrlEncode(password, p, sizeof(p));
	int len = _snprintf(body, sizeof(body), "username=%s&password=%s", u, p);
	int status = HttpsRequest("POST", "/api/mc/login",
		"Content-Type: application/x-www-form-urlencoded\r\n", body, len, resp, sizeof(resp), NULL);
	SecureZeroMemory(p, sizeof(p));
	SecureZeroMemory(body, sizeof(body));
	char token[128], username[64];
	if (status != 200 || !KvGet(resp, "token", token, sizeof(token)) || !KvGet(resp, "username", username, sizeof(username))) {
		ErrorFrom(status, resp, err, errCap);
		return false;
	}
	strncpy(g_token, token, sizeof(g_token) - 1);
	g_token[sizeof(g_token) - 1] = 0;
	strncpy(g_username, username, sizeof(g_username) - 1);
	g_username[sizeof(g_username) - 1] = 0;
	// As registered (older servers don't send it back: keep what was typed).
	if (!KvGet(resp, "email", g_email, sizeof(g_email)) || g_email[0] == 0)
		strncpy(g_email, email, sizeof(g_email) - 1);
	g_email[sizeof(g_email) - 1] = 0;
	nSettings::set_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_TOKEN", g_token);
	nSettings::set_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_USER", g_username);
	nSettings::set_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_EMAIL", g_email);
	ApplyDiscord(resp);
	return true;
}

void n02_wecamp_logout() {
	if (g_token[0]) {
		char headers[256], resp[256];
		AuthHeader(headers, sizeof(headers), NULL);
		HttpsRequest("POST", "/api/mc/logout", headers, NULL, 0, resp, sizeof(resp), NULL);
	}
	g_token[0] = 0;
	g_username[0] = 0;
	g_email[0] = 0;
	g_discord_linked = false;
	g_discord_name[0] = 0;
	nSettings::set_int_in(N02_WECAMP_SECTION, (char*)"DISCORD_LINKED", 0);
	nSettings::set_str_in(N02_WECAMP_SECTION, (char*)"DISCORD_NAME", (char*)"");
	nSettings::set_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_TOKEN", (char*)"");
	nSettings::set_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_USER", (char*)"");
	nSettings::set_str_in(N02_WECAMP_SECTION, (char*)"WECAMP_EMAIL", (char*)"");
}

bool n02_wecamp_get_ticket(const char* room, char* ticket, int ticketCap, char* err, int errCap) {
	if (ticket && ticketCap > 0) ticket[0] = 0;
	if (!n02_wecamp_logged_in()) {
		_snprintf(err, errCap, "Voce nao esta conectado a sua conta WE Camp.");
		err[errCap - 1] = 0;
		return false;
	}
	char r[160], body[200], resp[512], headers[512];
	UrlEncode(room, r, sizeof(r));
	int len = _snprintf(body, sizeof(body), "room=%s", r);
	AuthHeader(headers, sizeof(headers), "Content-Type: application/x-www-form-urlencoded\r\n");
	int status = HttpsRequest("POST", "/api/mc/ticket", headers, body, len, resp, sizeof(resp), NULL);
	if (status != 200 || !KvGet(resp, "ticket", ticket, ticketCap)) {
		ErrorFrom(status, resp, err, errCap);
		return false;
	}
	return true;
}

bool n02_wecamp_verify_ticket(const char* ticket, const char* room, char* username, int usernameCap, char* err, int errCap) {
	if (username && usernameCap > 0) username[0] = 0;
	char t[200], r[160], body[400], resp[512], headers[512];
	UrlEncode(ticket, t, sizeof(t));
	UrlEncode(room, r, sizeof(r));
	int len = _snprintf(body, sizeof(body), "ticket=%s&room=%s", t, r);
	AuthHeader(headers, sizeof(headers), "Content-Type: application/x-www-form-urlencoded\r\n");
	int status = HttpsRequest("POST", "/api/mc/verify-ticket", headers, body, len, resp, sizeof(resp), NULL);
	if (status != 200 || !KvGet(resp, "username", username, usernameCap) || username[0] == 0) {
		ErrorFrom(status, resp, err, errCap);
		return false;
	}
	return true;
}

bool n02_wecamp_checkout(const char* contentId, const char* gameName, const char* players,
	n02_wecamp_checkout_result* out, char* err, int errCap) {
	memset(out, 0, sizeof(*out));
	if (!n02_wecamp_logged_in()) {
		_snprintf(err, errCap, "Voce nao esta conectado a sua conta WE Camp.");
		err[errCap - 1] = 0;
		return false;
	}
	char c[64], g[400], pl[600], body[1200], resp[2048], headers[512];
	UrlEncode(contentId, c, sizeof(c));
	UrlEncode(gameName ? gameName : "", g, sizeof(g));
	UrlEncode(players, pl, sizeof(pl));
	int len = _snprintf(body, sizeof(body), "content_id=%s&game_name=%s&players=%s", c, g, pl);
	AuthHeader(headers, sizeof(headers), "Content-Type: application/x-www-form-urlencoded\r\n");
	int status = HttpsRequest("POST", "/api/mc/checkout", headers, body, len, resp, sizeof(resp), NULL);
	char value[128];
	if (status != 200 || !KvGet(resp, "slots", value, sizeof(value))) {
		KvGet(resp, "error", value, sizeof(value));
		if (strcmp(value, "missing_account") == 0) {
			char who[256];
			KvGet(resp, "message", who, sizeof(who));
			_snprintf(err, errCap, "Sem conta WE Camp confirmada: %s", who);
			err[errCap - 1] = 0;
		} else {
			ErrorFrom(status, resp, err, errCap);
		}
		return false;
	}
	out->slots = atoi(value);
	if (out->slots < 1 || out->slots > 2) {
		_snprintf(err, errCap, "Resposta invalida do WE Camp.");
		err[errCap - 1] = 0;
		return false;
	}
	for (int s = 0; s < out->slots; s++) {
		char key[32];
		_snprintf(key, sizeof(key), "slot%d_player", s + 1);
		KvGet(resp, key, out->player[s], sizeof(out->player[s]));
		_snprintf(key, sizeof(key), "slot%d_sha256", s + 1);
		KvGet(resp, key, out->sha256[s], sizeof(out->sha256[s]));
		_snprintf(key, sizeof(key), "slot%d_version", s + 1);
		KvGet(resp, key, value, sizeof(value));
		out->version[s] = atoi(value);
		if (strlen(out->sha256[s]) != 64 || out->player[s][0] == 0) {
			_snprintf(err, errCap, "Resposta invalida do WE Camp.");
			err[errCap - 1] = 0;
			return false;
		}
	}
	return true;
}

bool n02_wecamp_get_card(const char* sha256, char* buf, bool useApiKey) {
	char path[128], headers[512];
	_snprintf(path, sizeof(path), "/api/mc/card/%s", sha256);
	path[sizeof(path) - 1] = 0;
	if (useApiKey)
		_snprintf(headers, sizeof(headers), "X-Api-Key: %s\r\n", N02_STREAM_DEFAULT_API_KEY);
	else
		AuthHeader(headers, sizeof(headers), NULL);
	headers[sizeof(headers) - 1] = 0;
	int len = 0;
	int status = HttpsRequest("GET", path, headers, NULL, 0, buf, N02_MCD_SIZE + 1, &len);
	return status == 200 && len == N02_MCD_SIZE && buf[0] == 'M' && buf[1] == 'C';
}

bool n02_wecamp_commit(const char* contentId, const char* slotPlayer, const char* baseSha256,
	const char* players, const char* data, char* status, int statusCap, int* version) {
	if (status && statusCap > 0) status[0] = 0;
	if (version) *version = 0;
	char c[64], sp[128], pl[600], path[1100], headers[512], resp[1024];
	UrlEncode(contentId, c, sizeof(c));
	UrlEncode(slotPlayer, sp, sizeof(sp));
	UrlEncode(players, pl, sizeof(pl));
	_snprintf(path, sizeof(path), "/api/mc/commit?content_id=%s&slot_player=%s&base_sha256=%s&players=%s",
		c, sp, baseSha256, pl);
	path[sizeof(path) - 1] = 0;
	AuthHeader(headers, sizeof(headers), "Content-Type: application/octet-stream\r\n");
	int http = HttpsRequest("POST", path, headers, data, N02_MCD_SIZE, resp, sizeof(resp), NULL);
	char value[64];
	if (status) {
		if (!KvGet(resp, "status", status, statusCap)) {
			if (!KvGet(resp, "error", status, statusCap))
				_snprintf(status, statusCap, http ? "http_%d" : "offline", http);
		}
	}
	if (version && KvGet(resp, "version", value, sizeof(value)))
		*version = atoi(value);
	return http == 200;
}

bool n02_wecamp_voice_join(const char* players, char* status, int statusCap, char* url, int urlCap,
	char* appUrl, int appUrlCap, char* errCode, int errCodeCap, char* err, int errCap) {
	status[0] = url[0] = appUrl[0] = errCode[0] = err[0] = 0;
	if (!n02_wecamp_logged_in()) {
		_snprintf(errCode, errCodeCap, "not_logged_in");
		errCode[errCodeCap - 1] = 0;
		return false;
	}
	char pl[600], body[700], resp[1024], headers[512];
	UrlEncode(players, pl, sizeof(pl));
	int len = _snprintf(body, sizeof(body), "players=%s", pl);
	AuthHeader(headers, sizeof(headers), "Content-Type: application/x-www-form-urlencoded\r\n");
	int status_ = HttpsRequest("POST", "/api/voice/join", headers, body, len, resp, sizeof(resp), NULL);
	if (status_ != 200 || !KvGet(resp, "status", status, statusCap)) {
		KvGet(resp, "error", errCode, errCodeCap);
		ErrorFrom(status_, resp, err, errCap);
		return false;
	}
	KvGet(resp, "url", url, urlCap);
	KvGet(resp, "app_url", appUrl, appUrlCap);
	return true;
}

struct RefreshJob {
	HWND notify;
	UINT msg;
};

static DWORD WINAPI RefreshDiscordThread(LPVOID param) {
	RefreshJob* job = (RefreshJob*)param;
	if (n02_wecamp_refresh_discord() && job->notify && IsWindow(job->notify))
		PostMessage(job->notify, job->msg, 0, 0);
	delete job;
	return 0;
}

void n02_wecamp_refresh_discord_async(HWND notify, UINT msg) {
	if (!n02_wecamp_logged_in())
		return;
	RefreshJob* job = new RefreshJob;
	job->notify = notify;
	job->msg = msg;
	HANDLE t = CreateThread(NULL, 0, RefreshDiscordThread, job, 0, NULL);
	if (t)
		CloseHandle(t);
	else
		delete job;
}
