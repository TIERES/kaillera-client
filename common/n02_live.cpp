#include "n02_live.h"
#include "n02_wecamp.h"
#include "n02_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One state waiting to go (malloc'd form body), the worker's wake-up event
// and whether the worker is running - all under g_lock.
static CRITICAL_SECTION g_lock;
static HANDLE g_wake;
static char* g_pending;
static bool g_pending_final;
static bool g_running;
// The last state queued for the match being played (a copy) - until its
// final; n02_live_left() sends it again saying the player left.
static char* g_last;
// Reconectar: the replay being resumed (session id, "" = none) - sent with
// every state as "resume" (n02_live_set_resume()).
static char g_resume[64];
static void Kick();

static struct LiveInit {
	LiveInit() {
		InitializeCriticalSection(&g_lock);
		g_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
	}
} g_live_init;

// The worker leaves after this long with nothing to send (the next state
// starts another one): no thread is left waiting between matches.
#define N02_LIVE_IDLE_MS 30000
// A final state is tried this many times (the card's result).
#define N02_LIVE_FINAL_TRIES 4

static void UrlEncodeTo(char* out, size_t* o, const char* in) {
	static const char hex[] = "0123456789ABCDEF";
	for (const unsigned char* p = (const unsigned char*)in; *p; p++) {
		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) {
			out[(*o)++] = (char)*p;
		} else {
			out[(*o)++] = '%';
			out[(*o)++] = hex[*p >> 4];
			out[(*o)++] = hex[*p & 15];
		}
	}
}

static DWORD WINAPI LiveThread(LPVOID) {
	for (;;) {
		DWORD waited = WaitForSingleObject(g_wake, N02_LIVE_IDLE_MS);
		EnterCriticalSection(&g_lock);
		char* body = g_pending;
		bool final = g_pending_final;
		g_pending = NULL;
		g_pending_final = false;
		if (body == NULL && waited == WAIT_TIMEOUT) {
			g_running = false;
			LeaveCriticalSection(&g_lock);
			return 0;
		}
		LeaveCriticalSection(&g_lock);
		if (body == NULL)
			continue;
		for (int attempt = 0; attempt < (final ? N02_LIVE_FINAL_TRIES : 1); attempt++) {
			int status = n02_wecamp_live_post(body, (int)strlen(body));
			if (status == 200 || (status >= 400 && status < 500) || !n02_wecamp_logged_in())
				break; // sent, refused for good (bad state, login refused) or logged out
			Sleep(3000);
		}
		free(body);
	}
}

void n02_live_submit(const char* stateJson, const char* players, const char* reporter, const char* room, bool final) {
	if (stateJson == NULL || players == NULL || reporter == NULL)
		return;
	char resume[sizeof(g_resume)];
	EnterCriticalSection(&g_lock);
	memcpy(resume, g_resume, sizeof(resume));
	LeaveCriticalSection(&g_lock);
	size_t cap = 3 * (strlen(stateJson) + strlen(players) + strlen(reporter) + (room ? strlen(room) : 0) + strlen(resume)) + 80;
	char* body = (char*)malloc(cap);
	if (body == NULL)
		return;
	size_t o = 0;
	memcpy(body + o, "state=", 6); o += 6;
	UrlEncodeTo(body, &o, stateJson);
	memcpy(body + o, "&players=", 9); o += 9;
	UrlEncodeTo(body, &o, players);
	memcpy(body + o, "&reporter=", 10); o += 10;
	UrlEncodeTo(body, &o, reporter);
	memcpy(body + o, "&room=", 6); o += 6;
	UrlEncodeTo(body, &o, room ? room : "");
	if (resume[0]) {
		memcpy(body + o, "&resume=", 8); o += 8;
		UrlEncodeTo(body, &o, resume);
	}
	body[o] = 0;

	EnterCriticalSection(&g_lock);
	free(g_last);
	g_last = final ? NULL : _strdup(body); // after the final there's nothing to leave
	if (g_pending != NULL && g_pending_final && !final) {
		free(body); // the end of the match still waiting to go wins
	} else {
		free(g_pending);
		g_pending = body;
		g_pending_final = final;
	}
	Kick();
}

void n02_live_left() {
	EnterCriticalSection(&g_lock);
	char* last = g_last;
	g_last = NULL;
	char* body = NULL;
	// Not after the match's final (nor with it still waiting to go).
	if (last != NULL && !(g_pending != NULL && g_pending_final)) {
		size_t n = strlen(last);
		body = (char*)malloc(n + sizeof("&left=1"));
		if (body != NULL) {
			memcpy(body, last, n);
			memcpy(body + n, "&left=1", sizeof("&left=1"));
			free(g_pending);
			g_pending = body;
			g_pending_final = true; // tried like a final, and never replaced
		}
	}
	free(last);
	if (body == NULL) {
		LeaveCriticalSection(&g_lock);
		return;
	}
	Kick();
}

void n02_live_set_resume(const char* sessionId) {
	EnterCriticalSection(&g_lock);
	strncpy(g_resume, sessionId ? sessionId : "", sizeof(g_resume) - 1);
	g_resume[sizeof(g_resume) - 1] = 0;
	LeaveCriticalSection(&g_lock);
}

// A Reconectar notice (malloc'd form body), its own short-lived thread: a
// few tries, then it's gone - "start"/"cancel" are rare and must not be
// replaced by the states the live worker queues.
#define N02_LIVE_RECONNECT_TRIES 3

static DWORD WINAPI ReconnectThread(LPVOID param) {
	char* body = (char*)param;
	for (int attempt = 0; attempt < N02_LIVE_RECONNECT_TRIES; attempt++) {
		int status = n02_wecamp_live_reconnect_post(body, (int)strlen(body));
		if (status == 200 || (status >= 400 && status < 500) || !n02_wecamp_logged_in())
			break; // sent, refused for good or logged out
		Sleep(3000);
	}
	free(body);
	return 0;
}

void n02_live_reconnect(const char* sessionId, const char* phase) {
	if (sessionId == NULL || !sessionId[0] || phase == NULL || !n02_wecamp_logged_in())
		return;
	size_t cap = 3 * (strlen(sessionId) + strlen(phase)) + 32;
	char* body = (char*)malloc(cap);
	if (body == NULL)
		return;
	size_t o = 0;
	memcpy(body + o, "session=", 8); o += 8;
	UrlEncodeTo(body, &o, sessionId);
	memcpy(body + o, "&phase=", 7); o += 7;
	UrlEncodeTo(body, &o, phase);
	body[o] = 0;
	HANDLE t = n02_create_thread(ReconnectThread, body);
	if (t)
		CloseHandle(t);
	else
		free(body);
}

// (g_lock held - released here) Starts the worker if it isn't running and wakes it.
static void Kick() {
	bool start = !g_running;
	if (start)
		g_running = true;
	LeaveCriticalSection(&g_lock);
	if (start) {
		HANDLE t = n02_create_thread(LiveThread, NULL);
		if (t) {
			CloseHandle(t);
		} else {
			EnterCriticalSection(&g_lock);
			g_running = false;
			LeaveCriticalSection(&g_lock);
			return;
		}
	}
	SetEvent(g_wake);
}
