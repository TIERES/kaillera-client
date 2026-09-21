#include "n02_update.h"
#include "n02_stream.h" // N02_STREAM_DEFAULT_HOST/PORT - same community server, different path
#include "n02_version.h"

#include <windows.h>
#include "k_socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A DLL always knows its own architecture at compile time - no runtime
// detection needed, and no risk of ever requesting the wrong build.
#ifdef _WIN64
#define N02_UPDATE_ARCH "x64"
#else
#define N02_UPDATE_ARCH "x86"
#endif

// A kailleraclient.dll build is a few hundred KB; refuse anything wildly
// larger than that as a sanity check against a misbehaving/compromised
// response rather than trust the server-reported size unconditionally.
#define N02_UPDATE_MAX_SIZE (16 * 1024 * 1024)

///////////////////////////////////////////////////////////////////////////////
// CRC32 (standard IEEE 802.3 / zlib polynomial - matches Python's
// zlib.crc32(), which is what app/updates.py uses server-side) - just an
// integrity check against a truncated/corrupted download before this code
// commits to swapping the running DLL out for it, not a security boundary.
///////////////////////////////////////////////////////////////////////////////

static unsigned long Crc32(const unsigned char* data, int len) {
	static unsigned long table[256];
	static bool tableInit = false;
	if (!tableInit) {
		for (unsigned long i = 0; i < 256; i++) {
			unsigned long c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		tableInit = true;
	}
	unsigned long crc = 0xFFFFFFFFUL;
	for (int n = 0; n < len; n++)
		crc = table[(crc ^ (unsigned char)data[n]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFUL;
}

///////////////////////////////////////////////////////////////////////////////
// Blocking HTTP GET helpers - raw sockets, no TLS. Style mirrors
// n02_watch.cpp's HttpGetBody(), including the same DNS-caching and
// bounded-connect() fixes (a stalled connect() here would freeze
// kailleraInit() itself, before any UI even shows).
///////////////////////////////////////////////////////////////////////////////

static char g_update_resolved_host[256] = { 0 };
static struct in_addr g_update_resolved_addr;
static bool g_update_resolved_valid = false;

static SOCKET ConnectWithTimeout(const char* host, int port, int timeoutMs) {
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
		if (!g_update_resolved_valid || strcmp(g_update_resolved_host, host) != 0) {
			struct hostent* he = gethostbyname(host);
			if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
				closesocket(s);
				return INVALID_SOCKET;
			}
			g_update_resolved_addr = *(struct in_addr*)he->h_addr_list[0];
			strncpy(g_update_resolved_host, host, sizeof(g_update_resolved_host) - 1);
			g_update_resolved_host[sizeof(g_update_resolved_host) - 1] = 0;
			g_update_resolved_valid = true;
		}
		server.sin_addr = g_update_resolved_addr;
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
		return INVALID_SOCKET;
	}
	return s;
}

static bool SendGetRequest(SOCKET s, const char* host, const char* path) {
	char header[512];
	int headerLen = _snprintf(header, sizeof(header),
		"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
	if (headerLen < 0 || headerLen >= (int)sizeof(header))
		return false;
	return send(s, header, headerLen, 0) == headerLen;
}

// One-shot small response (the "version\tsize\tcrc32" line) - mirrors
// n02_watch.cpp's HttpGetBody. Returns body length, or -1 on any
// network/non-2xx error.
static int HttpGetSmall(const char* host, int port, const char* path, char* outBuf, int outCap) {
	SOCKET s = ConnectWithTimeout(host, port, 3000);
	if (s == INVALID_SOCKET) return -1;
	if (!SendGetRequest(s, host, path)) { closesocket(s); return -1; }

	char recvBuf[4096];
	int total = 0;
	while (total < (int)sizeof(recvBuf)) {
		int r = recv(s, recvBuf + total, sizeof(recvBuf) - total, 0);
		if (r <= 0) break;
		total += r;
	}
	closesocket(s);

	if (total < 12 || strncmp(recvBuf, "HTTP/1.", 7) != 0 || recvBuf[9] != '2')
		return -1;

	char* bodyStart = NULL;
	for (int i = 0; i + 3 < total; i++) {
		if (recvBuf[i] == '\r' && recvBuf[i + 1] == '\n' && recvBuf[i + 2] == '\r' && recvBuf[i + 3] == '\n') {
			bodyStart = recvBuf + i + 4;
			break;
		}
	}
	if (bodyStart == NULL) return -1;

	int bodyLen = total - (int)(bodyStart - recvBuf);
	if (bodyLen < 0) bodyLen = 0;
	if (bodyLen > outCap) bodyLen = outCap;
	if (bodyLen > 0) memcpy(outBuf, bodyStart, bodyLen);
	return bodyLen;
}

// Full download (the DLL itself) - loops recv() until the peer closes,
// since the payload doesn't fit in one TCP read like the small responses
// above do. outCap must already be sized to the expected download (the
// caller gets that from /updates/latest before calling this).
static int HttpGetToBuffer(const char* host, int port, const char* path, char* outBuf, int outCap) {
	SOCKET s = ConnectWithTimeout(host, port, 5000);
	if (s == INVALID_SOCKET) return -1;
	if (!SendGetRequest(s, host, path)) { closesocket(s); return -1; }

	char headBuf[8192];
	int headTotal = 0;
	char* bodyStart = NULL;
	while (headTotal < (int)sizeof(headBuf)) {
		int r = recv(s, headBuf + headTotal, sizeof(headBuf) - headTotal, 0);
		if (r <= 0) { closesocket(s); return -1; }
		headTotal += r;
		for (int i = 0; i + 3 < headTotal; i++) {
			if (headBuf[i] == '\r' && headBuf[i + 1] == '\n' && headBuf[i + 2] == '\r' && headBuf[i + 3] == '\n') {
				bodyStart = headBuf + i + 4;
				break;
			}
		}
		if (bodyStart != NULL) break;
	}
	if (bodyStart == NULL || headTotal < 12 || strncmp(headBuf, "HTTP/1.", 7) != 0 || headBuf[9] != '2') {
		closesocket(s);
		return -1;
	}

	int total = 0;
	int bodyInHead = headTotal - (int)(bodyStart - headBuf);
	if (bodyInHead > 0) {
		if (bodyInHead > outCap) bodyInHead = outCap;
		memcpy(outBuf, bodyStart, bodyInHead);
		total = bodyInHead;
	}
	while (total < outCap) {
		int r = recv(s, outBuf + total, outCap - total, 0);
		if (r <= 0) break;
		total += r;
	}
	closesocket(s);
	return total;
}

///////////////////////////////////////////////////////////////////////////////
// Swap: renames a staged kailleraclient.dll.new over the current DLL. Works
// even while this DLL is loaded and running - the Windows loader opens DLL
// files with share-delete/rename permission, so the currently-executing
// code keeps running unaffected from the renamed-away file, and whichever
// process loads "kailleraclient.dll" next picks up the new one. No reboot,
// no helper process needed - just a message telling the user to relaunch.
///////////////////////////////////////////////////////////////////////////////

static bool SwapInPendingUpdate(const char* dllPath) {
	char newPath[MAX_PATH];
	char oldPath[MAX_PATH];
	_snprintf(newPath, sizeof(newPath), "%s.new", dllPath);
	_snprintf(oldPath, sizeof(oldPath), "%s.old", dllPath);
	newPath[sizeof(newPath) - 1] = 0;
	oldPath[sizeof(oldPath) - 1] = 0;

	if (GetFileAttributesA(newPath) == INVALID_FILE_ATTRIBUTES)
		return false; // nothing staged

	DeleteFileA(oldPath); // best-effort - clear the way for the rename below

	if (!MoveFileExA(dllPath, oldPath, MOVEFILE_REPLACE_EXISTING))
		return false; // locked more strictly than usual this time - retry next launch

	if (!MoveFileExA(newPath, dllPath, MOVEFILE_REPLACE_EXISTING)) {
		// Try to put the previously-working DLL back rather than leave the
		// slot empty.
		MoveFileExA(oldPath, dllPath, MOVEFILE_REPLACE_EXISTING);
		return false;
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Public API
///////////////////////////////////////////////////////////////////////////////

void n02_update_check_and_prompt() {
	char selfPath[MAX_PATH];
	HMODULE hSelf = NULL;
	GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&n02_update_check_and_prompt, &hSelf);
	if (hSelf == NULL || GetModuleFileNameA(hSelf, selfPath, sizeof(selfPath)) == 0)
		return; // can't locate our own file on disk - skip silently

	// Finish off anything staged by a run that couldn't complete the swap
	// immediately, before possibly staging another one below.
	SwapInPendingUpdate(selfPath);

	if (strcmp(N02_VERSION, "dev") == 0)
		return; // local/dev build - not tagged, nothing to compare against

	char path[128];
	_snprintf(path, sizeof(path), "/updates/latest?arch=%s", N02_UPDATE_ARCH);

	char resp[512];
	int n = HttpGetSmall(N02_STREAM_DEFAULT_HOST, N02_STREAM_DEFAULT_PORT, path, resp, sizeof(resp) - 1);
	if (n <= 0)
		return; // no update published yet, or a network hiccup - stay quiet either way
	resp[n] = 0;

	char version[64];
	int size = 0;
	char crcHex[16];
	if (sscanf(resp, "%63[^\t]\t%d\t%15s", version, &size, crcHex) != 3)
		return;
	if (size <= 0 || size > N02_UPDATE_MAX_SIZE)
		return;
	if (strcmp(version, N02_VERSION) == 0)
		return; // already current

	char msg[512];
	wsprintf(msg,
		"Uma nova versao do Kaillera esta disponivel:\n\n  Atual: %s\n  Nova:  %s\n\nDeseja baixar e atualizar agora?",
		N02_VERSION, version);
	if (MessageBox(NULL, msg, "Atualizacao disponivel", MB_YESNO | MB_ICONQUESTION) != IDYES)
		return;

	char* buf = (char*)malloc(size);
	if (buf == NULL)
		return;

	_snprintf(path, sizeof(path), "/updates/download/%s", N02_UPDATE_ARCH);
	int got = HttpGetToBuffer(N02_STREAM_DEFAULT_HOST, N02_STREAM_DEFAULT_PORT, path, buf, size);

	unsigned long expectedCrc = strtoul(crcHex, NULL, 16);
	bool ok = (got == size) && (Crc32((unsigned char*)buf, size) == expectedCrc);
	if (!ok) {
		free(buf);
		MessageBox(NULL,
			"Falha ao baixar a atualizacao (dados incompletos ou corrompidos). Tente novamente mais tarde.",
			"Atualizacao", MB_OK | MB_ICONWARNING);
		return;
	}

	char newPath[MAX_PATH];
	_snprintf(newPath, sizeof(newPath), "%s.new", selfPath);
	newPath[sizeof(newPath) - 1] = 0;

	HANDLE f = CreateFileA(newPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		free(buf);
		return;
	}
	DWORD written = 0;
	WriteFile(f, buf, size, &written, NULL);
	CloseHandle(f);
	free(buf);
	if ((int)written != size) {
		DeleteFileA(newPath);
		return;
	}

	if (SwapInPendingUpdate(selfPath)) {
		MessageBox(NULL,
			"Atualizacao instalada! Feche e abra o emulador novamente para usar a nova versao.",
			"Atualizacao", MB_OK | MB_ICONINFORMATION);
	} else {
		MessageBox(NULL,
			"Atualizacao baixada, mas nao foi possivel instalar agora (arquivo em uso). Feche e abra o emulador novamente para concluir.",
			"Atualizacao", MB_OK | MB_ICONINFORMATION);
	}
}
