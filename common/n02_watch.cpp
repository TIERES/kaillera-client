#include "n02_watch.h"
#include "n02_stream.h" // N02_STREAM_DEFAULT_HOST/PORT/API_KEY - same community server, different paths
#include "n02_replays.h" // n02_replays_request_live_state/live_state_ready/download_live_state - "Ir direto para o Ao Vivo!"

#include <windows.h>
#include "k_socket.h"
#include "nThread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Blocking HTTP GET helper - raw sockets, no TLS. Style mirrors
// HttpPostBytes() in n02_stream.cpp.
///////////////////////////////////////////////////////////////////////////////

// A single response (headers + body) never exceeds this - the server caps
// /spectate/stream chunks at 1MB (MAX_STREAM_CHUNK_BYTES in spectate.py).
#define N02_WATCH_RECV_CAP (1024 * 1024 + 8192)

// Cached DNS result for the (essentially static, community-server) stream
// host - see the matching comment in n02_stream.cpp's HttpPostBytes. This
// matters even more here: the watch thread below polls every 30-200ms, so
// a fresh gethostbyname() (and an unbounded connect()) on every single call
// stalls the spectator's live view far more often than the host's own
// once-per-300ms upload would hit the same issue.
static char g_watch_resolved_host[256] = { 0 };
static struct in_addr g_watch_resolved_addr;
static bool g_watch_resolved_valid = false;

// GETs `path`, copies the response body into outBuf (up to outCap bytes,
// truncated if larger - caller just polls again for the rest) and returns
// its length, or -1 on any network/non-2xx error. If outStatusHeader is
// given, copies the "X-Status" response header's value into it (empty
// string if absent).
static int HttpGetBody(const char* host, int port, const char* path, const char* apiKey, char* outBuf, int outCap, char* outStatusHeader, int outStatusHeaderCap) {
	if (host == NULL || host[0] == 0 || port <= 0)
		return -1;

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s == INVALID_SOCKET)
		return -1;

	sockaddr_in server;
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons((u_short)port);

	if (host[0] >= '0' && host[0] <= '9') {
		server.sin_addr.s_addr = inet_addr(host);
	} else {
		if (!g_watch_resolved_valid || strcmp(g_watch_resolved_host, host) != 0) {
			struct hostent* he = gethostbyname(host);
			if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
				closesocket(s);
				return -1;
			}
			g_watch_resolved_addr = *(struct in_addr*)he->h_addr_list[0];
			strncpy(g_watch_resolved_host, host, sizeof(g_watch_resolved_host) - 1);
			g_watch_resolved_host[sizeof(g_watch_resolved_host) - 1] = 0;
			g_watch_resolved_valid = true;
		}
		server.sin_addr = g_watch_resolved_addr;
	}

	int timeoutMs = 3000;
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

	// connect() itself ignores SO_SNDTIMEO/SO_RCVTIMEO on a blocking socket
	// and can otherwise stall for many seconds on a network blip - during
	// which the spectator's whole live view is frozen (player_MPV's refill
	// blocks the emulator's own thread waiting on this). Bound it via a
	// non-blocking connect, same as n02_stream.cpp's HttpPostBytes.
	u_long nonBlocking = 1;
	ioctlsocket(s, FIONBIO, &nonBlocking);

	bool connected = (connect(s, (struct sockaddr*)&server, sizeof(server)) == 0);
	if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
		fd_set wfds, efds;
		FD_ZERO(&wfds); FD_SET(s, &wfds);
		FD_ZERO(&efds); FD_SET(s, &efds);
		struct timeval tv;
		tv.tv_sec = timeoutMs / 1000;
		tv.tv_usec = (timeoutMs % 1000) * 1000;
		connected = (select(0, NULL, &wfds, &efds, &tv) > 0) && !FD_ISSET(s, &efds);
	}

	u_long blocking = 0;
	ioctlsocket(s, FIONBIO, &blocking);

	if (!connected) {
		closesocket(s);
		return -1;
	}

	char apiKeyHeader[192];
	apiKeyHeader[0] = 0;
	if (apiKey != NULL && apiKey[0] != 0)
		_snprintf(apiKeyHeader, sizeof(apiKeyHeader), "X-Api-Key: %s\r\n", apiKey);

	char header[1024];
	int headerLen = _snprintf(header, sizeof(header),
		"GET %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"%s"
		"Connection: close\r\n"
		"\r\n",
		path, host, apiKeyHeader);
	if (headerLen < 0 || headerLen >= (int)sizeof(header)) {
		closesocket(s);
		return -1;
	}

	if (send(s, header, headerLen, 0) != headerLen) {
		closesocket(s);
		return -1;
	}

	char* recvBuf = (char*)malloc(N02_WATCH_RECV_CAP + 1);
	if (recvBuf == NULL) {
		closesocket(s);
		return -1;
	}
	int total = 0;
	while (total < N02_WATCH_RECV_CAP) {
		int r = recv(s, recvBuf + total, N02_WATCH_RECV_CAP - total, 0);
		if (r <= 0) break;
		total += r;
	}
	closesocket(s);
	recvBuf[total] = 0; // safe: allocated N02_WATCH_RECV_CAP+1, total <= N02_WATCH_RECV_CAP

	// Status line: reject anything that isn't 2xx.
	if (total < 12 || strncmp(recvBuf, "HTTP/1.", 7) != 0 || recvBuf[9] != '2') {
		free(recvBuf);
		return -1;
	}

	// Split headers/body on the blank line (byte-safe scan, doesn't rely on
	// the body itself being text).
	char* bodyStart = NULL;
	for (int i = 0; i + 3 < total; i++) {
		if (recvBuf[i] == '\r' && recvBuf[i + 1] == '\n' && recvBuf[i + 2] == '\r' && recvBuf[i + 3] == '\n') {
			bodyStart = recvBuf + i + 4;
			break;
		}
	}
	if (bodyStart == NULL) {
		free(recvBuf);
		return -1;
	}

	if (outStatusHeader != NULL && outStatusHeaderCap > 0) {
		outStatusHeader[0] = 0;
		char savedBodyByte = *bodyStart;
		*bodyStart = 0; // temporarily terminate so the header search can't run into binary body data
		const char* h = strstr(recvBuf, "X-Status:");
		*bodyStart = savedBodyByte; // restore before bodyLen/memcpy below read the real body
		if (h != NULL) {
			h += 9;
			while (*h == ' ') h++;
			int i = 0;
			while (h[i] != '\r' && h[i] != '\n' && h[i] != 0 && i < outStatusHeaderCap - 1) {
				outStatusHeader[i] = h[i];
				i++;
			}
			outStatusHeader[i] = 0;
		}
	}

	int bodyLen = total - (int)(bodyStart - recvBuf);
	if (bodyLen < 0) bodyLen = 0;
	if (bodyLen > outCap) bodyLen = outCap;
	if (bodyLen > 0)
		memcpy(outBuf, bodyStart, bodyLen);

	free(recvBuf);
	return bodyLen;
}

///////////////////////////////////////////////////////////////////////////////
// /spectate/lookup - resolve a room name to a session id
///////////////////////////////////////////////////////////////////////////////

static void UrlEncode(const char* in, char* out, int cap) {
	int o = 0;
	for (int i = 0; in[i] != 0 && o < cap - 4; i++) {
		unsigned char c = (unsigned char)in[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = "0123456789ABCDEF"[c >> 4];
			out[o++] = "0123456789ABCDEF"[c & 0xF];
		}
	}
	out[o] = 0;
}

// Pulls the string value of a top-level "key":"value" pair out of a small
// JSON object. Good enough for the server's own controlled output (no
// escaping to worry about); not a general JSON parser.
static bool JsonExtractString(const char* json, const char* key, char* out, int cap) {
	char pattern[64];
	_snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char* k = strstr(json, pattern);
	if (k == NULL) return false;
	const char* colon = strchr(k, ':');
	if (colon == NULL) return false;
	const char* q1 = strchr(colon, '"');
	if (q1 == NULL) return false;
	q1++;
	const char* q2 = strchr(q1, '"');
	if (q2 == NULL) return false;
	int len = (int)(q2 - q1);
	if (len >= cap) len = cap - 1;
	if (len < 0) len = 0;
	memcpy(out, q1, len);
	out[len] = 0;
	return len > 0;
}

bool n02_watch_lookup_session(const char* room, const char* owner, char* outSessionId, int cap) {
	char encodedRoom[384];
	UrlEncode(room, encodedRoom, sizeof(encodedRoom));

	char path[512];
	if (owner != NULL && owner[0] != 0) {
		char encodedOwner[192];
		UrlEncode(owner, encodedOwner, sizeof(encodedOwner));
		_snprintf(path, sizeof(path), "/spectate/lookup?room=%s&owner=%s", encodedRoom, encodedOwner);
	} else {
		_snprintf(path, sizeof(path), "/spectate/lookup?room=%s", encodedRoom);
	}

	char body[1024];
	int bodyLen = HttpGetBody(N02_STREAM_DEFAULT_HOST, N02_STREAM_DEFAULT_PORT, path, N02_STREAM_DEFAULT_API_KEY, body, sizeof(body) - 1, NULL, 0);
	if (bodyLen <= 0)
		return false;
	body[bodyLen] = 0;

	return JsonExtractString(body, "session_id", outSessionId, cap);
}

///////////////////////////////////////////////////////////////////////////////
// /spectate/stream - background prefetch thread + drain buffer
///////////////////////////////////////////////////////////////////////////////

static CRITICAL_SECTION g_watch_lock;
static bool g_watch_lock_init = false;
static char* g_watch_buf = NULL;
static int g_watch_buf_len = 0;
static int g_watch_buf_cap = 0;
static bool g_watch_finished = false;
static char g_watch_session_id[64] = { 0 };

static void WatchEnsureLock() {
	if (!g_watch_lock_init) {
		InitializeCriticalSection(&g_watch_lock);
		g_watch_lock_init = true;
	}
}

static void WatchBufAppend(const char* data, int len) {
	if (len <= 0) return;
	WatchEnsureLock();
	EnterCriticalSection(&g_watch_lock);
	if (g_watch_buf_len + len > g_watch_buf_cap) {
		int newCap = (g_watch_buf_cap > 0) ? g_watch_buf_cap : (64 * 1024);
		while (newCap < g_watch_buf_len + len)
			newCap *= 2;
		char* nb = (char*)realloc(g_watch_buf, newCap);
		if (nb != NULL) {
			g_watch_buf = nb;
			g_watch_buf_cap = newCap;
		}
	}
	if (g_watch_buf_len + len <= g_watch_buf_cap) {
		memcpy(g_watch_buf + g_watch_buf_len, data, len);
		g_watch_buf_len += len;
	}
	// else: realloc failed, drop this chunk - next poll re-requests the same
	// server-side offset (we only advance our offset by what we buffered).
	LeaveCriticalSection(&g_watch_lock);
}

// Set by n02_watch_start()/n02_watch_restart_from_offset() right before
// creating the thread below - read once at the top of run().
static int g_watch_start_offset = 0;

class WatchThread : public nThread {
public:
	volatile bool running;
	volatile bool stop_requested;

	void run() {
		running = true;
		char sessionId[64];
		strncpy(sessionId, g_watch_session_id, sizeof(sessionId) - 1);
		sessionId[sizeof(sessionId) - 1] = 0;

		static char chunk[1024 * 1024 + 4096];
		int offset = g_watch_start_offset;

		while (!stop_requested) {
			char path[256];
			_snprintf(path, sizeof(path), "/spectate/stream/%s?offset=%d", sessionId, offset);

			char status[32];
			int n = HttpGetBody(N02_STREAM_DEFAULT_HOST, N02_STREAM_DEFAULT_PORT, path, N02_STREAM_DEFAULT_API_KEY, chunk, sizeof(chunk), status, sizeof(status));
			if (n > 0) {
				WatchBufAppend(chunk, n);
				offset += n;
			}

			bool isFinished = (n >= 0) && (strcmp(status, "finished") == 0);
			if (isFinished && n <= 0) {
				WatchEnsureLock();
				EnterCriticalSection(&g_watch_lock);
				g_watch_finished = true;
				LeaveCriticalSection(&g_watch_lock);
				break;
			}

			Sleep(n > 0 ? 30 : 200);
		}
		running = false;
	}
} g_watch_thread;

void n02_watch_start(const char* sessionId) {
	n02_watch_stop();

	WatchEnsureLock();
	EnterCriticalSection(&g_watch_lock);
	g_watch_buf_len = 0;
	g_watch_finished = false;
	LeaveCriticalSection(&g_watch_lock);

	strncpy(g_watch_session_id, sessionId, sizeof(g_watch_session_id) - 1);
	g_watch_session_id[sizeof(g_watch_session_id) - 1] = 0;

	g_watch_start_offset = 0;
	g_watch_thread.stop_requested = false;
	g_watch_thread.create();
}

void n02_watch_restart_from_offset(int byteOffset) {
	if (g_watch_session_id[0] == 0)
		return; // no session started yet - nothing to restart

	n02_watch_stop();

	WatchEnsureLock();
	EnterCriticalSection(&g_watch_lock);
	g_watch_buf_len = 0;
	g_watch_finished = false;
	LeaveCriticalSection(&g_watch_lock);

	g_watch_start_offset = byteOffset;
	g_watch_thread.stop_requested = false;
	g_watch_thread.create();
}

bool n02_watch_request_state() {
	if (g_watch_session_id[0] == 0)
		return false;
	return n02_replays_request_live_state(g_watch_session_id);
}

bool n02_watch_state_ready() {
	if (g_watch_session_id[0] == 0)
		return false;
	return n02_replays_live_state_ready(g_watch_session_id);
}

int n02_watch_download_state(void* outBuffer, int bufferCap, int* outFrameIndex, int* outByteOffset) {
	if (g_watch_session_id[0] == 0)
		return -1;

	int frameIndex = 0, byteOffset = 0, size = 0;
	void* state = n02_replays_download_live_state(g_watch_session_id, &frameIndex, &byteOffset, &size);
	if (state == NULL)
		return -1;

	int copyLen = min(size, bufferCap);
	memcpy(outBuffer, state, copyLen);
	free(state);

	if (outFrameIndex) *outFrameIndex = frameIndex;
	if (outByteOffset) *outByteOffset = byteOffset;
	return copyLen;
}

int n02_watch_pull(char* outBuf, int outCap, bool blockIfLive) {
	WatchEnsureLock();
	for (;;) {
		EnterCriticalSection(&g_watch_lock);
		int n = g_watch_buf_len;
		if (n > outCap) n = outCap;
		if (n > 0) {
			memcpy(outBuf, g_watch_buf, n);
			memmove(g_watch_buf, g_watch_buf + n, g_watch_buf_len - n);
			g_watch_buf_len -= n;
		}
		bool finished = g_watch_finished && g_watch_buf_len == 0;
		LeaveCriticalSection(&g_watch_lock);

		if (n > 0) return n;
		if (!blockIfLive || finished) return 0;
		Sleep(20);
	}
}

void n02_watch_stop() {
	if (g_watch_thread.running) {
		g_watch_thread.stop_requested = true;
		for (int i = 0; i < 40 && g_watch_thread.running; i++)
			Sleep(50);
		if (g_watch_thread.running)
			g_watch_thread.destroy();
	}
}
