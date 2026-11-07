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
	size_t cap = 3 * (strlen(stateJson) + strlen(players) + strlen(reporter) + (room ? strlen(room) : 0)) + 64;
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
	body[o] = 0;

	EnterCriticalSection(&g_lock);
	if (g_pending != NULL && g_pending_final && !final) {
		free(body); // the end of the match still waiting to go wins
	} else {
		free(g_pending);
		g_pending = body;
		g_pending_final = final;
	}
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
