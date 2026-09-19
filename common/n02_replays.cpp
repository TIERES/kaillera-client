#include "n02_replays.h"
#include "n02_stream.h" // N02_STREAM_DEFAULT_HOST/PORT/API_KEY - same community server, different paths

#include <windows.h>
#include "k_socket.h"

#include <stdio.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Shared connect + request-send helper. Returns a connected, request-sent
// socket, or INVALID_SOCKET on any failure. Style mirrors HttpPostBytes() in
// n02_stream.cpp and HttpGetBody() in n02_watch.cpp.
///////////////////////////////////////////////////////////////////////////////

static SOCKET ConnectAndGet(const char* host, int port, const char* path, const char* apiKey, int timeoutMs) {
	if (host == NULL || host[0] == 0 || port <= 0)
		return INVALID_SOCKET;

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s == INVALID_SOCKET)
		return INVALID_SOCKET;

	sockaddr_in server;
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons((u_short)port);

	if (host[0] >= '0' && host[0] <= '9') {
		server.sin_addr.s_addr = inet_addr(host);
	} else {
		struct hostent* he = gethostbyname(host);
		if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
			closesocket(s);
			return INVALID_SOCKET;
		}
		server.sin_addr = *(struct in_addr*)he->h_addr_list[0];
	}

	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

	if (connect(s, (struct sockaddr*)&server, sizeof(server)) != 0) {
		closesocket(s);
		return INVALID_SOCKET;
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
	if (headerLen < 0 || headerLen >= (int)sizeof(header) || send(s, header, headerLen, 0) != headerLen) {
		closesocket(s);
		return INVALID_SOCKET;
	}

	return s;
}

// Reads from s until the blank line ending the response headers. On success,
// returns true with any body bytes that arrived in the same recv() copied
// into leftoverBody (up to leftoverCap, actual length in *leftoverLen) and
// the status code's first digit's class checked (2xx required).
static bool ReadHttpHeaders(SOCKET s, char* leftoverBody, int leftoverCap, int* leftoverLen) {
	*leftoverLen = 0;

	char buf[8192];
	int total = 0;
	int headerEnd = -1;
	while (total < (int)sizeof(buf)) {
		int r = recv(s, buf + total, sizeof(buf) - total, 0);
		if (r <= 0) return false;
		total += r;
		for (int i = 0; i + 3 < total; i++) {
			if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
				headerEnd = i + 4;
				break;
			}
		}
		if (headerEnd >= 0) break;
	}
	if (headerEnd < 0) return false;
	if (total < 12 || strncmp(buf, "HTTP/1.", 7) != 0 || buf[9] != '2') return false;

	int bodyLen = total - headerEnd;
	if (bodyLen > 0) {
		if (bodyLen > leftoverCap) bodyLen = leftoverCap;
		memcpy(leftoverBody, buf + headerEnd, bodyLen);
	}
	*leftoverLen = bodyLen;
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// GET /replays/list.txt - small tab-separated response, buffered in memory.
///////////////////////////////////////////////////////////////////////////////

#define N02_REPLAYS_LIST_RECV_CAP (64 * 1024)

static void SplitTsvLine(char* line, N02ReplayEntry* out) {
	memset(out, 0, sizeof(*out));
	char* fields[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
	int n = 0;
	char* p = line;
	fields[n++] = p;
	while (*p != 0 && n < 7) {
		if (*p == '\t') {
			*p = 0;
			fields[n++] = p + 1;
		}
		p++;
	}
	if (n < 7) return; // malformed line, leave the entry zeroed (caller skips empty session_id)

	strncpy(out->session_id, fields[0], sizeof(out->session_id) - 1);
	strncpy(out->when, fields[1], sizeof(out->when) - 1);
	strncpy(out->game_name, fields[2], sizeof(out->game_name) - 1);
	strncpy(out->player_names, fields[3], sizeof(out->player_names) - 1);
	out->duration_seconds = atoi(fields[4]);
	strncpy(out->download_name, fields[5], sizeof(out->download_name) - 1);
	out->size_bytes = atoi(fields[6]);
}

int n02_replays_fetch_list(N02ReplayEntry* out, int maxEntries) {
	if (maxEntries > N02_REPLAYS_MAX_ENTRIES) maxEntries = N02_REPLAYS_MAX_ENTRIES;
	if (maxEntries <= 0) return 0;

	char path[64];
	_snprintf(path, sizeof(path), "/replays/list.txt?limit=%d", maxEntries);

	SOCKET s = ConnectAndGet(N02_STREAM_DEFAULT_HOST, N02_STREAM_DEFAULT_PORT, path, N02_STREAM_DEFAULT_API_KEY, 3000);
	if (s == INVALID_SOCKET) return 0;

	char* body = (char*)malloc(N02_REPLAYS_LIST_RECV_CAP + 1);
	if (body == NULL) {
		closesocket(s);
		return 0;
	}

	int bodyLen = 0;
	if (!ReadHttpHeaders(s, body, N02_REPLAYS_LIST_RECV_CAP, &bodyLen)) {
		free(body);
		closesocket(s);
		return 0;
	}
	while (bodyLen < N02_REPLAYS_LIST_RECV_CAP) {
		int r = recv(s, body + bodyLen, N02_REPLAYS_LIST_RECV_CAP - bodyLen, 0);
		if (r <= 0) break;
		bodyLen += r;
	}
	closesocket(s);
	body[bodyLen] = 0;

	int count = 0;
	char* line = body;
	while (*line != 0 && count < maxEntries) {
		char* eol = strchr(line, '\n');
		if (eol != NULL) *eol = 0;
		int lineLen = (int)strlen(line);
		if (lineLen > 0 && line[lineLen - 1] == '\r') line[lineLen - 1] = 0;

		if (line[0] != 0) {
			SplitTsvLine(line, &out[count]);
			if (out[count].session_id[0] != 0) count++;
		}

		if (eol == NULL) break;
		line = eol + 1;
	}

	free(body);
	return count;
}

///////////////////////////////////////////////////////////////////////////////
// GET /replays/<session_id>/download - streamed straight to disk, no size cap.
///////////////////////////////////////////////////////////////////////////////

bool n02_replays_download(const char* sessionId, const char* destPath) {
	char path[128];
	_snprintf(path, sizeof(path), "/replays/%s/download", sessionId);

	SOCKET s = ConnectAndGet(N02_STREAM_DEFAULT_HOST, N02_STREAM_DEFAULT_PORT, path, N02_STREAM_DEFAULT_API_KEY, 5000);
	if (s == INVALID_SOCKET) return false;

	char leftover[8192];
	int leftoverLen = 0;
	if (!ReadHttpHeaders(s, leftover, sizeof(leftover), &leftoverLen)) {
		closesocket(s);
		return false;
	}

	HANDLE out = CreateFile(destPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (out == INVALID_HANDLE_VALUE) {
		closesocket(s);
		return false;
	}

	DWORD written;
	bool ok = true;
	if (leftoverLen > 0)
		ok = WriteFile(out, leftover, leftoverLen, &written, NULL) != 0;

	char chunk[64 * 1024];
	while (ok) {
		int r = recv(s, chunk, sizeof(chunk), 0);
		if (r <= 0) break;
		ok = WriteFile(out, chunk, r, &written, NULL) != 0;
	}

	CloseHandle(out);
	closesocket(s);
	return ok;
}
