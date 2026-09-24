#include "n02_stream.h"

#include <windows.h>
#include "k_socket.h"
#include "nThread.h"
#include "../stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

///////////////////////////////////////////////////////////////////////////////
// Endpoint configuration
///////////////////////////////////////////////////////////////////////////////

static char g_stream_host[256] = { 0 };
static int  g_stream_port = 0;
static char g_stream_path[256] = { 0 };
static char g_stream_api_key[128] = { 0 };

void n02_stream_configure(const char* host, int port, const char* path, const char* apiKey) {
	strncpy(g_stream_host, (host != NULL) ? host : "", sizeof(g_stream_host) - 1);
	g_stream_host[sizeof(g_stream_host) - 1] = 0;
	g_stream_port = port;
	strncpy(g_stream_path, (path != NULL && *path != 0) ? path : "/", sizeof(g_stream_path) - 1);
	g_stream_path[sizeof(g_stream_path) - 1] = 0;
	strncpy(g_stream_api_key, (apiKey != NULL) ? apiKey : "", sizeof(g_stream_api_key) - 1);
	g_stream_api_key[sizeof(g_stream_api_key) - 1] = 0;
}

void n02_stream_configure_from_text(const char* rawEndpoint, const char* defaultHost, int defaultPort, const char* defaultPath, const char* defaultApiKey) {
	char buf[256];
	strncpy(buf, (rawEndpoint != NULL) ? rawEndpoint : "", sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;

	char apiKey[128];
	apiKey[0] = 0;
	char* atPos = strchr(buf, '@');
	if (atPos != NULL) {
		int keyLen = (int)(atPos - buf);
		if (keyLen >= (int)sizeof(apiKey)) keyLen = sizeof(apiKey) - 1;
		memcpy(apiKey, buf, keyLen);
		apiKey[keyLen] = 0;
		memmove(buf, atPos + 1, strlen(atPos + 1) + 1);
	} else {
		strncpy(apiKey, (defaultApiKey != NULL) ? defaultApiKey : "", sizeof(apiKey) - 1);
		apiKey[sizeof(apiKey) - 1] = 0;
	}

	char path[256];
	char* slashPos = strchr(buf, '/');
	if (slashPos != NULL) {
		strncpy(path, slashPos, sizeof(path) - 1);
		path[sizeof(path) - 1] = 0;
		*slashPos = 0;
	} else {
		strncpy(path, (defaultPath != NULL) ? defaultPath : "/", sizeof(path) - 1);
		path[sizeof(path) - 1] = 0;
	}

	int port = defaultPort;
	char* colonPos = strchr(buf, ':');
	if (colonPos != NULL) {
		if (colonPos[1] != 0)
			port = atoi(colonPos + 1);
		*colonPos = 0;
	}

	char host[256];
	strncpy(host, buf, sizeof(host) - 1);
	host[sizeof(host) - 1] = 0;
	if (host[0] == 0)
		strncpy(host, (defaultHost != NULL) ? defaultHost : "", sizeof(host) - 1);
	host[sizeof(host) - 1] = 0;

	n02_stream_configure(host, port, path, apiKey);
}

///////////////////////////////////////////////////////////////////////////////
// Record queue - single producer (the thread driving kailleraModifyPlayValues
// and the chat/drop callbacks), single consumer (StreamThread below).
///////////////////////////////////////////////////////////////////////////////

#define N02_STREAM_RECORD_MAX 512
// At ~60 frames/sec, 512 slots is only ~8.5s of buffering - too little to
// survive a real network stall (e.g. a slow DNS lookup or a dropped
// connection needing a retry) without silently dropping the oldest frames.
// 8192 slots is ~2.3 minutes, a much safer margin; cost is ~4MB static.
#define N02_STREAM_QUEUE_CAP  8192

struct StreamRecord {
	int len; // total bytes in data, including the leading type byte
	char data[N02_STREAM_RECORD_MAX];
};

static CRITICAL_SECTION g_stream_lock;
static bool g_stream_lock_init = false;
static StreamRecord g_stream_queue[N02_STREAM_QUEUE_CAP];
static int g_stream_q_head = 0; // next write index
static int g_stream_q_count = 0;

// Stream offset, in the server's own <session_id>.krec.part coordinates
// (KRC1 header included - StreamThread always sends it first), right past
// the last record handed to StreamEnqueue() - i.e. exactly where the *next*
// record pushed will land once StreamThread gets it there. Advanced
// synchronously by whoever pushes, which for frames is the emulator's own
// thread (kailleraModifyPlayValues()) - so when that same thread reads it
// right after a core_serialize(), every frame the state has already applied
// is before this offset and every frame it hasn't is after it. That's what
// makes "Ir direto para o Ao Vivo!" frame-exact - see n02_stream_upload_state().
// Counting bytes already POSTed instead (what this used to do) lagged a
// whole batch plus the POST itself behind, so a spectator re-applied a few
// dozen inputs the state had already consumed and stayed shifted by that
// many frames for good - a permanent desync, not a self-correcting one.
// Protected by g_stream_lock.
static int g_stream_bytes_enqueued = 0;

static void StreamEnsureLock() {
	if (!g_stream_lock_init) {
		InitializeCriticalSection(&g_stream_lock);
		g_stream_lock_init = true;
	}
}

static void StreamEnqueue(const char* bytes, int len) {
	if (len <= 0)
		return;
	if (len > N02_STREAM_RECORD_MAX)
		len = N02_STREAM_RECORD_MAX;
	StreamEnsureLock();
	EnterCriticalSection(&g_stream_lock);
	int slot = (g_stream_q_head) % N02_STREAM_QUEUE_CAP;
	if (g_stream_q_count == N02_STREAM_QUEUE_CAP)
		g_stream_bytes_enqueued -= g_stream_queue[slot].len; // full - this slot is the oldest record, about to be dropped unsent
	memcpy(g_stream_queue[slot].data, bytes, len);
	g_stream_queue[slot].len = len;
	g_stream_bytes_enqueued += len;
	g_stream_q_head = (g_stream_q_head + 1) % N02_STREAM_QUEUE_CAP;
	if (g_stream_q_count < N02_STREAM_QUEUE_CAP)
		g_stream_q_count++;
	// else: queue was already full, oldest record just got overwritten (drop-oldest)
	LeaveCriticalSection(&g_stream_lock);
}

// Drains as much as fits from the queue (oldest first) into outBuf
// (caller-provided, outCap bytes). Returns the number of bytes written; any
// records that didn't fit stay queued for the next call.
static int StreamDrain(char* outBuf, int outCap) {
	StreamEnsureLock();
	EnterCriticalSection(&g_stream_lock);
	int count = g_stream_q_count;
	int tail = (g_stream_q_head - count + N02_STREAM_QUEUE_CAP) % N02_STREAM_QUEUE_CAP;
	int written = 0;
	int consumed = 0;
	for (int i = 0; i < count; i++) {
		StreamRecord* rec = &g_stream_queue[(tail + i) % N02_STREAM_QUEUE_CAP];
		if (written + rec->len > outCap)
			break;
		memcpy(outBuf + written, rec->data, rec->len);
		written += rec->len;
		consumed++;
	}
	g_stream_q_count -= consumed;
	LeaveCriticalSection(&g_stream_lock);
	return written;
}

static int CopyCStringBounded(char* dst, const char* src, int cap) {
	if (src == NULL) src = "";
	int len = (int)strlen(src) + 1;
	if (len > cap) len = cap;
	if (len <= 0) return 0;
	memcpy(dst, src, len - 1);
	dst[len - 1] = 0;
	return len;
}

///////////////////////////////////////////////////////////////////////////////
// Session state
///////////////////////////////////////////////////////////////////////////////

static bool g_session_active = false;   // a session has been started
static bool g_session_ended = false;    // End() was called, final POST pending
static char g_session_id[64] = { 0 };
static char g_session_owner[64] = { 0 }; // hosting user's name - see X-Owner-Name below
static unsigned int g_session_sequence = 0;
static char g_session_header[400];      // KRC1-style header, sent with sequence 0

static void BuildSessionHeader(const char* appName, const char* gameName, int playerno, int numplayers, char playerNames[4][32]) {
	memset(g_session_header, 0, sizeof(g_session_header));
	char* p = g_session_header;
	memcpy(p, "KRC1", 4); p += 4;

	char appBuf[128]; memset(appBuf, 0, sizeof(appBuf));
	strncpy(appBuf, (appName != NULL) ? appName : "", sizeof(appBuf) - 1);
	memcpy(p, appBuf, 128); p += 128;

	char gameBuf[128]; memset(gameBuf, 0, sizeof(gameBuf));
	strncpy(gameBuf, (gameName != NULL) ? gameName : "", sizeof(gameBuf) - 1);
	memcpy(p, gameBuf, 128); p += 128;

	time_t now = time(NULL);
	memcpy(p, &now, 4); p += 4;
	memcpy(p, &playerno, 4); p += 4;
	memcpy(p, &numplayers, 4); p += 4;

	if (playerNames != NULL) {
		memcpy(p, playerNames, 128);
	}
	p += 128;
}

///////////////////////////////////////////////////////////////////////////////
// HTTP POST helper - blocking, raw sockets, no TLS. Only ever called from
// StreamThread's own worker thread, so blocking here is fine. Style mirrors
// DownloadListToBuffer() in kaillera_ui_mslist.cpp.
///////////////////////////////////////////////////////////////////////////////

// Cached DNS result for the (essentially static, community-server) stream
// host - gethostbyname() used to run on *every* POST, and a slow/flaky
// resolver stalling that blocking call was enough by itself to stall the
// whole StreamThread for seconds at a time (see the queue-overflow comment
// on g_stream_queue below).
static char g_resolved_host[256] = { 0 };
static struct in_addr g_resolved_addr;
static bool g_resolved_valid = false;

static bool HttpPostBytes(const char* host, int port, const char* path, const char* apiKey, const char* sessionId, const char* ownerName, unsigned int sequence, bool sessionEnd, const char* body, int bodyLen) {
	if (host == NULL || host[0] == 0 || port <= 0)
		return false;

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s == INVALID_SOCKET)
		return false;

	sockaddr_in server;
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons((u_short)port);

	if (host[0] >= '0' && host[0] <= '9') {
		server.sin_addr.s_addr = inet_addr(host);
	} else {
		if (!g_resolved_valid || strcmp(g_resolved_host, host) != 0) {
			struct hostent* he = gethostbyname(host);
			if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
				closesocket(s);
				return false;
			}
			g_resolved_addr = *(struct in_addr*)he->h_addr_list[0];
			strncpy(g_resolved_host, host, sizeof(g_resolved_host) - 1);
			g_resolved_host[sizeof(g_resolved_host) - 1] = 0;
			g_resolved_valid = true;
		}
		server.sin_addr = g_resolved_addr;
	}

	int timeoutMs = 2000;
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

	// connect() itself ignores SO_SNDTIMEO/SO_RCVTIMEO on a blocking socket
	// and can otherwise stall for many seconds (Windows' default TCP connect
	// retry/timeout) on a network blip - during which nothing drains the
	// frame queue below. Bound it explicitly via a non-blocking connect.
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
		return false;
	}

	char apiKeyHeader[192];
	apiKeyHeader[0] = 0;
	if (apiKey != NULL && apiKey[0] != 0)
		_snprintf(apiKeyHeader, sizeof(apiKeyHeader), "X-Api-Key: %s\r\n", apiKey);

	char ownerHeader[192];
	ownerHeader[0] = 0;
	if (ownerName != NULL && ownerName[0] != 0)
		_snprintf(ownerHeader, sizeof(ownerHeader), "X-Owner-Name: %s\r\n", ownerName);

	char header[1024];
	int headerLen = _snprintf(header, sizeof(header),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Type: application/octet-stream\r\n"
		"Content-Length: %d\r\n"
		"X-Session-Id: %s\r\n"
		"X-Sequence: %u\r\n"
		"%s"
		"%s"
		"%s"
		"Connection: close\r\n"
		"\r\n",
		path, host, bodyLen, sessionId, sequence,
		sessionEnd ? "X-Session-End: true\r\n" : "",
		ownerHeader,
		apiKeyHeader);
	if (headerLen < 0 || headerLen >= (int)sizeof(header)) {
		closesocket(s);
		return false;
	}

	if (send(s, header, headerLen, 0) != headerLen) {
		closesocket(s);
		return false;
	}
	if (bodyLen > 0) {
		int sent = 0;
		while (sent < bodyLen) {
			int r = send(s, body + sent, bodyLen - sent, 0);
			if (r <= 0) {
				closesocket(s);
				return false;
			}
			sent += r;
		}
	}

	// Drain (and discard) whatever the server replies, just to let it close
	// the connection cleanly; we don't need the response body.
	char discard[512];
	DWORD start = GetTickCount();
	while (GetTickCount() - start < (DWORD)timeoutMs) {
		int r = recv(s, discard, sizeof(discard), 0);
		if (r <= 0)
			break;
	}

	closesocket(s);
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Background sender thread
///////////////////////////////////////////////////////////////////////////////

#define N02_STREAM_BATCH_MS 300

class StreamThread : public nThread {
public:
	volatile bool running;
	volatile bool stop_requested;

	void run() {
		running = true;
		char sendBuf[64 * 1024 + sizeof(g_session_header)];

		// A batch that failed to POST stays here (same bytes, same
		// X-Sequence) and is retried next tick instead of being dropped -
		// the receiving server just appends bytes as they arrive with no
		// gap detection (see spectate.py's /spectate/ingest), so silently
		// losing a batch used to permanently corrupt the record framing for
		// the rest of the session. New frames keep queuing (bounded,
		// drop-oldest - see StreamEnqueue) while a batch is stuck retrying.
		bool havePending = false;
		char* pendingPayload = NULL;
		int pendingLen = 0;
		bool pendingEnded = false;

		while (!stop_requested) {
			Sleep(N02_STREAM_BATCH_MS);

			if (!havePending) {
				bool ended = g_session_ended;
				int n = StreamDrain(sendBuf + sizeof(g_session_header), sizeof(sendBuf) - sizeof(g_session_header));

				bool isFirst = (g_session_sequence == 0);
				char* payload = sendBuf + sizeof(g_session_header);
				int payloadLen = n;
				if (isFirst) {
					memcpy(sendBuf, g_session_header, sizeof(g_session_header));
					payload = sendBuf;
					payloadLen = n + (int)sizeof(g_session_header);
				}

				if (payloadLen <= 0 && !ended)
					continue; // nothing to send yet

				pendingPayload = payload;
				pendingLen = payloadLen;
				pendingEnded = ended;
				havePending = true;
			}

			if (HttpPostBytes(g_stream_host, g_stream_port, g_stream_path, g_stream_api_key, g_session_id, g_session_owner, g_session_sequence, pendingEnded, pendingPayload, pendingLen)) {
				g_session_sequence++;
				havePending = false;
				if (pendingEnded) {
					g_session_active = false;
					break;
				}
			} else {
				StatsAppendLine("stream: POST failed (seq %u, %d bytes) - will retry", g_session_sequence, pendingLen);
			}
		}
		running = false;
	}
} g_stream_thread;

static void StreamThreadStop() {
	if (g_stream_thread.running) {
		g_stream_thread.stop_requested = true;
		for (int i = 0; i < 20 && g_stream_thread.running; i++)
			Sleep(50);
		if (g_stream_thread.running)
			g_stream_thread.destroy();
	}
}

///////////////////////////////////////////////////////////////////////////////
// "Ir direto para o Ao Vivo!" - host-side half (poll for a pending request,
// capture+upload a state). See common/n02_watch.cpp for the spectator-side
// half (request + download) and spectate.py's /spectate/<id>/state-request
// and /state routes for the wire contract both sides agree on.
///////////////////////////////////////////////////////////////////////////////

// Simple blocking GET, reusing g_resolved_host/g_resolved_addr above (same
// endpoint as the ingest POSTs). Returns the body length copied into outBuf
// (up to outCap), or -1 on any network/non-2xx error.
static int HttpGetSimple(const char* host, int port, const char* path, const char* apiKey, char* outBuf, int outCap) {
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
		if (!g_resolved_valid || strcmp(g_resolved_host, host) != 0) {
			struct hostent* he = gethostbyname(host);
			if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
				closesocket(s);
				return -1;
			}
			g_resolved_addr = *(struct in_addr*)he->h_addr_list[0];
			strncpy(g_resolved_host, host, sizeof(g_resolved_host) - 1);
			g_resolved_host[sizeof(g_resolved_host) - 1] = 0;
			g_resolved_valid = true;
		}
		server.sin_addr = g_resolved_addr;
	}

	int timeoutMs = 2000;
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

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
		"GET %s HTTP/1.1\r\nHost: %s\r\n%sConnection: close\r\n\r\n",
		path, host, apiKeyHeader);
	if (headerLen < 0 || headerLen >= (int)sizeof(header) || send(s, header, headerLen, 0) != headerLen) {
		closesocket(s);
		return -1;
	}

	char recvBuf[8192];
	int total = 0;
	while (total < (int)sizeof(recvBuf) - 1) {
		int r = recv(s, recvBuf + total, sizeof(recvBuf) - 1 - total, 0);
		if (r <= 0) break;
		total += r;
	}
	closesocket(s);
	recvBuf[total] = 0;

	if (total < 12 || strncmp(recvBuf, "HTTP/1.", 7) != 0 || recvBuf[9] != '2')
		return -1;

	char* bodyStart = strstr(recvBuf, "\r\n\r\n");
	if (bodyStart == NULL)
		return -1;
	bodyStart += 4;

	int bodyLen = total - (int)(bodyStart - recvBuf);
	if (bodyLen < 0) bodyLen = 0;
	if (bodyLen > outCap) bodyLen = outCap;
	if (bodyLen > 0)
		memcpy(outBuf, bodyStart, bodyLen);
	return bodyLen;
}

// Simple blocking POST with a raw body (no multipart/form encoding) - used
// for the state-upload POST.
static bool HttpPostSimple(const char* host, int port, const char* path, const char* apiKey, const void* body, int bodyLen, int timeoutMs) {
	if (host == NULL || host[0] == 0 || port <= 0)
		return false;

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s == INVALID_SOCKET)
		return false;

	sockaddr_in server;
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons((u_short)port);

	if (host[0] >= '0' && host[0] <= '9') {
		server.sin_addr.s_addr = inet_addr(host);
	} else {
		if (!g_resolved_valid || strcmp(g_resolved_host, host) != 0) {
			struct hostent* he = gethostbyname(host);
			if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
				closesocket(s);
				return false;
			}
			g_resolved_addr = *(struct in_addr*)he->h_addr_list[0];
			strncpy(g_resolved_host, host, sizeof(g_resolved_host) - 1);
			g_resolved_host[sizeof(g_resolved_host) - 1] = 0;
			g_resolved_valid = true;
		}
		server.sin_addr = g_resolved_addr;
	}

	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

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
		return false;
	}

	char apiKeyHeader[192];
	apiKeyHeader[0] = 0;
	if (apiKey != NULL && apiKey[0] != 0)
		_snprintf(apiKeyHeader, sizeof(apiKeyHeader), "X-Api-Key: %s\r\n", apiKey);

	char header[512];
	int headerLen = _snprintf(header, sizeof(header),
		"POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n%sConnection: close\r\n\r\n",
		path, host, bodyLen, apiKeyHeader);
	if (headerLen < 0 || headerLen >= (int)sizeof(header) || send(s, header, headerLen, 0) != headerLen) {
		closesocket(s);
		return false;
	}
	if (bodyLen > 0) {
		int sent = 0;
		while (sent < bodyLen) {
			int r = send(s, (const char*)body + sent, bodyLen - sent, 0);
			if (r <= 0) {
				closesocket(s);
				return false;
			}
			sent += r;
		}
	}

	char discard[512];
	DWORD start = GetTickCount();
	while (GetTickCount() - start < (DWORD)timeoutMs) {
		int r = recv(s, discard, sizeof(discard), 0);
		if (r <= 0) break;
	}
	closesocket(s);
	return true;
}

// Background thread doing all of this feature's host-side network I/O. Both
// the pending-request poll and the (multi-MB) state upload used to run
// synchronously inside the frontend's per-frame tick - a GET every few
// seconds for the whole match, plus a full upload whenever a spectator
// asked - each one freezing the emulator for the entire round trip, and in
// Kaillera's lockstep every other player's game along with it. The
// frontend-facing calls (n02_stream_check_state_requested()/
// n02_stream_upload_state() below) now only ever flip these flags/hand off
// this buffer, all under g_stream_lock.

// Rate-limits the poll - hitting the server any faster would be pointless
// spam, and a spectator's request just waits up to this long to be noticed.
#define N02_STREAM_STATE_POLL_INTERVAL_MS 3000
// Generous on purpose: a PCSX ReARMed state is several MB, and this no
// longer holds up anyone's game while it waits.
#define N02_STREAM_STATE_UPLOAD_TIMEOUT_MS 20000

// Set by the first n02_stream_check_state_requested() call of a session -
// StateThread doesn't poll at all for a frontend that never asks (any
// emulator other than retroarch-k3), same as before this ran on its own
// thread.
static volatile bool g_state_frontend_polling = false;
// StateThread saw the server's pending flag; the frontend hasn't picked it up yet.
static volatile bool g_state_request_pending = false;
// [frame_index:int32 LE][byte_offset:int32 LE][state bytes], handed off by
// n02_stream_upload_state() for StateThread to POST. A newer one replaces
// one not picked up yet - only the latest ever matters to a spectator.
static char* g_state_upload_buf = NULL;
static int g_state_upload_len = 0;

class StateThread : public nThread {
public:
	volatile bool running;
	volatile bool stop_requested;

	void run() {
		DWORD lastPoll = 0;

		while (!stop_requested && g_session_active && !g_session_ended) {
			Sleep(100);

			EnterCriticalSection(&g_stream_lock);
			char* upload = g_state_upload_buf;
			int uploadLen = g_state_upload_len;
			g_state_upload_buf = NULL;
			g_state_upload_len = 0;
			LeaveCriticalSection(&g_stream_lock);

			if (upload != NULL) {
				char path[128];
				_snprintf(path, sizeof(path), "/spectate/%s/state", g_session_id);
				if (!HttpPostSimple(g_stream_host, g_stream_port, path, g_stream_api_key, upload, uploadLen, N02_STREAM_STATE_UPLOAD_TIMEOUT_MS))
					StatsAppendLine("stream: state upload failed (%d bytes)", uploadLen);
				free(upload);
				// The upload clears the server's pending flag - wait a full
				// interval before asking again, or we'd re-see the request
				// this one just serviced if the server hasn't committed yet.
				// A failed upload leaves the flag set, so the next poll just
				// triggers a fresh capture.
				lastPoll = GetTickCount();
				continue;
			}

			if (!g_state_frontend_polling || g_state_request_pending)
				continue; // nobody to hand a request to, or the last one still hasn't been picked up

			DWORD now = GetTickCount();
			if (lastPoll != 0 && now - lastPoll < N02_STREAM_STATE_POLL_INTERVAL_MS)
				continue;
			lastPoll = now;

			char path[128];
			_snprintf(path, sizeof(path), "/spectate/%s/state-request", g_session_id);
			char body[128];
			int n = HttpGetSimple(g_stream_host, g_stream_port, path, g_stream_api_key, body, sizeof(body) - 1);
			if (n <= 0)
				continue;
			body[n] = 0;
			if (strstr(body, "\"pending\": true") != NULL || strstr(body, "\"pending\":true") != NULL) {
				EnterCriticalSection(&g_stream_lock);
				g_state_request_pending = true;
				LeaveCriticalSection(&g_stream_lock);
			}
		}
		running = false;
	}
} g_state_thread;

static void StateThreadStop() {
	if (g_state_thread.running) {
		g_state_thread.stop_requested = true;
		for (int i = 0; i < 20 && g_state_thread.running; i++)
			Sleep(50);
		if (g_state_thread.running) {
			g_state_thread.destroy();
			g_state_thread.running = false;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// Public API
///////////////////////////////////////////////////////////////////////////////

void n02_stream_start_session(const char* appName, const char* gameName, int playerno, int numplayers, char playerNames[4][32], const char* ownerName) {
	if (g_stream_host[0] == 0) {
		StatsAppendLine("stream: enabled but no endpoint host configured, not streaming");
		return;
	}

	// Make sure a previous session's threads (if any) have fully stopped
	// before we reset shared state for the new one.
	StreamThreadStop();
	StateThreadStop();

	StreamEnsureLock();
	EnterCriticalSection(&g_stream_lock);
	g_stream_q_head = 0;
	g_stream_q_count = 0;
	g_stream_bytes_enqueued = sizeof(g_session_header); // StreamThread's first POST always leads with it
	g_state_frontend_polling = false;
	g_state_request_pending = false;
	free(g_state_upload_buf);
	g_state_upload_buf = NULL;
	g_state_upload_len = 0;
	LeaveCriticalSection(&g_stream_lock);

	_snprintf(g_session_id, sizeof(g_session_id), "%lu-%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)time(NULL));
	g_session_id[sizeof(g_session_id) - 1] = 0;
	strncpy(g_session_owner, (ownerName != NULL) ? ownerName : "", sizeof(g_session_owner) - 1);
	g_session_owner[sizeof(g_session_owner) - 1] = 0;
	g_session_sequence = 0;
	g_session_ended = false;
	BuildSessionHeader(appName, gameName, playerno, numplayers, playerNames);
	g_session_active = true;

	g_stream_thread.stop_requested = false;
	if (g_stream_thread.create() == 0) {
		StatsAppendLine("stream: failed to start sender thread");
		g_session_active = false;
		return;
	}

	// running is set here rather than at the top of run() so a
	// StateThreadStop() racing the thread's own startup can't miss it.
	g_state_thread.stop_requested = false;
	g_state_thread.running = true;
	if (g_state_thread.create() == 0) {
		g_state_thread.running = false;
		StatsAppendLine("stream: failed to start state thread - \"Ir direto para o Ao Vivo!\" won't be serviced this session");
	}
}

void n02_stream_push_frame(const void* values, int len) {
	if (!g_session_active || g_session_ended)
		return;
	if (len < 0) len = 0;
	if (len > N02_STREAM_RECORD_MAX - 3) len = N02_STREAM_RECORD_MAX - 3;
	char rec[N02_STREAM_RECORD_MAX];
	rec[0] = 0x12;
	short slen = (short)len;
	memcpy(rec + 1, &slen, 2);
	if (len > 0)
		memcpy(rec + 3, values, len);
	StreamEnqueue(rec, 3 + len);
}

void n02_stream_push_chat(const char* nick, const char* text) {
	if (!g_session_active || g_session_ended)
		return;
	char rec[N02_STREAM_RECORD_MAX];
	rec[0] = 0x08;
	int p = 1;
	p += CopyCStringBounded(rec + p, nick, sizeof(rec) - p);
	p += CopyCStringBounded(rec + p, text, sizeof(rec) - p);
	StreamEnqueue(rec, p);
}

void n02_stream_push_drop(const char* nick, int playernb) {
	if (!g_session_active || g_session_ended)
		return;
	char rec[N02_STREAM_RECORD_MAX];
	rec[0] = 0x14;
	int p = 1;
	p += CopyCStringBounded(rec + p, nick, sizeof(rec) - p);
	if (p + 4 <= (int)sizeof(rec)) {
		memcpy(rec + p, &playernb, 4);
		p += 4;
	}
	StreamEnqueue(rec, p);
}

// Called every frame by the frontend - never touches the network itself
// (see StateThread above), so it's just a flag check.
bool n02_stream_check_state_requested() {
	if (!g_session_active || g_session_ended)
		return false;

	g_state_frontend_polling = true;
	if (!g_state_request_pending)
		return false;

	EnterCriticalSection(&g_stream_lock);
	bool requested = g_state_request_pending;
	g_state_request_pending = false;
	LeaveCriticalSection(&g_stream_lock);
	return requested;
}

void n02_stream_upload_state(int frameIndex, const void* data, int size) {
	if (!g_session_active || g_session_ended || data == NULL || size <= 0)
		return;

	// Read right away - must be called on the thread pushing frames, right
	// after the core_serialize() that produced `data` and before the next
	// frame's input is pushed; see g_stream_bytes_enqueued's own comment.
	EnterCriticalSection(&g_stream_lock);
	int byteOffset = g_stream_bytes_enqueued;
	LeaveCriticalSection(&g_stream_lock);

	int headerLen = 8; // [frame_index:int32 LE][byte_offset:int32 LE]
	char* buf = (char*)malloc(headerLen + size);
	if (buf == NULL)
		return;
	memcpy(buf, &frameIndex, 4);
	memcpy(buf + 4, &byteOffset, 4);
	memcpy(buf + headerLen, data, size);

	// Hand off to StateThread - the actual upload happens off this thread.
	EnterCriticalSection(&g_stream_lock);
	char* superseded = g_state_upload_buf;
	g_state_upload_buf = buf;
	g_state_upload_len = headerLen + size;
	LeaveCriticalSection(&g_stream_lock);
	free(superseded);
}

void n02_stream_end_session() {
	if (g_session_active)
		g_session_ended = true;
}

void n02_stream_shutdown() {
	n02_stream_end_session();
	StreamThreadStop();
	StateThreadStop();
}
